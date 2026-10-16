#include "edgerpc/shared_buffer.hpp"
#include "edgerpc/transport.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace edgerpc {
namespace {

[[noreturn]] void system_failure(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void validate_size(std::size_t bytes) {
    if (bytes < 8 || bytes > kMaxSharedBytes) throw ProtocolError("shared region must be 8 bytes through 16 MiB");
}

std::string unique_name() {
    std::random_device random;
    std::ostringstream name;
    name << "/erpc_" << std::hex << std::setfill('0');
    for (int i = 0; i < 3; ++i) name << std::setw(8) << static_cast<std::uint32_t>(random());
    return name.str();
}

void close_on_exec(int fd) {
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) system_failure("fcntl shared memory");
}

struct stat inspect(int fd) {
    struct stat information{};
    if (::fstat(fd, &information) < 0) system_failure("fstat shared memory");
    bool valid_type = S_ISREG(information.st_mode);
#ifdef __APPLE__
    // XNU's PSXSHM stat reports permission bits without an S_IFREG type.
    valid_type = valid_type || (information.st_mode & S_IFMT) == 0;
#endif
    if (!valid_type || information.st_uid != ::geteuid() || (information.st_mode & 0777) != 0600)
        throw ProtocolError("shared memory must be a same-user object with mode 0600 (preserve owner rw in umask)");
    return information;
}

void* map(int fd, std::size_t bytes) {
    auto* result = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (result == MAP_FAILED) system_failure("mmap shared memory");
    return result;
}

} // namespace

SharedBuffer SharedBuffer::create(std::size_t bytes) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "Shared tensors require IEEE-754 binary32");
    validate_size(bytes);
    SharedBuffer result;
    UniqueFd fd;
    for (int attempt = 0; attempt < 16; ++attempt) {
        result.name_ = unique_name();
        fd = UniqueFd(::shm_open(result.name_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600));
        if (fd.get() >= 0) break;
        if (errno != EEXIST) system_failure("shm_open create");
    }
    if (fd.get() < 0) throw std::runtime_error("could not allocate a unique shared-memory name");
    result.owns_name_ = true;
    close_on_exec(fd.get());
    // fchmod is not supported on macOS PSXSHM descriptors. Validate creation mode instead.
    inspect(fd.get());
    if (::ftruncate(fd.get(), static_cast<off_t>(bytes)) < 0) system_failure("ftruncate shared memory");
    const auto information = inspect(fd.get());
    // macOS rounds ftruncate to a VM page; advertise the actual size, never weaken attach checks.
    if (information.st_size < 0 || static_cast<std::uint64_t>(information.st_size) < bytes ||
        static_cast<std::uint64_t>(information.st_size) > kMaxSharedBytes)
        throw ProtocolError("unsupported shared-memory allocation size");
    result.size_ = static_cast<std::size_t>(information.st_size);
#ifdef __linux__
    // tmpfs ftruncate alone is sparse: reserve backing storage before a write can SIGBUS.
    int error;
    do { error = ::posix_fallocate(fd.get(), 0, static_cast<off_t>(result.size_)); } while (error == EINTR);
    if (error != 0) throw std::system_error(error, std::generic_category(), "reserve shared memory");
#endif
    result.mapping_ = map(fd.get(), result.size_);
    return result;
}

SharedBuffer SharedBuffer::open(const std::string& name, std::size_t expected_bytes) {
    validate_shared_name(name);
    validate_size(expected_bytes);
    UniqueFd fd(::shm_open(name.c_str(), O_RDWR, 0));
    if (fd.get() < 0) system_failure("shm_open attach");
    close_on_exec(fd.get());
    const auto information = inspect(fd.get());
    if (information.st_size < 0 || static_cast<std::uint64_t>(information.st_size) != expected_bytes)
        throw ProtocolError("shared-memory object size does not match descriptor");
    SharedBuffer result;
    result.name_ = name;
    result.size_ = expected_bytes;
    result.mapping_ = map(fd.get(), expected_bytes);
    return result;
}

SharedBuffer::~SharedBuffer() { reset(); }
SharedBuffer::SharedBuffer(SharedBuffer&& other) noexcept { swap(other); }
SharedBuffer& SharedBuffer::operator=(SharedBuffer&& other) noexcept {
    if (this != &other) { reset(); swap(other); }
    return *this;
}

void SharedBuffer::swap(SharedBuffer& other) noexcept {
    name_.swap(other.name_);
    std::swap(size_, other.size_);
    std::swap(mapping_, other.mapping_);
    std::swap(owns_name_, other.owns_name_);
}

void SharedBuffer::reset() noexcept {
    if (mapping_ != nullptr) ::munmap(mapping_, size_);
    if (owns_name_ && !name_.empty()) ::shm_unlink(name_.c_str());
    mapping_ = nullptr;
    size_ = 0;
    owns_name_ = false;
    name_.clear();
}

void SharedBuffer::unlink_name() {
    if (name_.empty()) throw std::runtime_error("shared buffer has been moved");
    if (::shm_unlink(name_.c_str()) < 0 && errno != ENOENT) system_failure("shm_unlink");
    owns_name_ = false;
}

void SharedBuffer::check_range(std::uint64_t offset, std::size_t elements) const {
    if (mapping_ == nullptr || elements > kMaxElements || offset % 4 != 0 ||
        offset > size_ || elements * 4ULL > size_ - offset)
        throw ProtocolError("shared tensor access is unaligned or out of bounds");
}

void SharedBuffer::write_tensor(std::uint64_t offset, const std::vector<float>& input) {
    check_range(offset, input.size());
    if (!input.empty()) std::memcpy(static_cast<std::uint8_t*>(mapping_) + offset, input.data(), input.size() * 4);
}

std::vector<float> SharedBuffer::read_tensor(std::uint64_t offset, std::size_t elements) const {
    check_range(offset, elements);
    std::vector<float> output(elements);
    if (elements != 0) std::memcpy(output.data(), static_cast<const std::uint8_t*>(mapping_) + offset, elements * 4);
    return output;
}

} // namespace edgerpc
