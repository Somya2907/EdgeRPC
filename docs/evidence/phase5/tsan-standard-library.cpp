// Standalone standard-library diagnostic: no EdgeRPC code or dependencies.
#include <atomic>
#include <cerrno>
#include <future>
#include <system_error>
#include <thread>
int main() {
    for (int n=0;n<1000;++n) {
        std::promise<int> promise;
        auto future=promise.get_future();
        std::thread sender([promise=std::move(promise)]() mutable {
            try { throw std::system_error(EFBIG,std::generic_category(),"allocation"); }
            catch (...) { promise.set_exception(std::current_exception()); }
        });
        try { future.get(); }
        catch (const std::system_error& error) {
            if (error.code().value()!=EFBIG) return 1;
        }
        sender.join();
    }
}
