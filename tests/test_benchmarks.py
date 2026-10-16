"""Synthetic fixtures test rejection/aggregation only; never publish them as evidence."""
import csv
import json
from pathlib import Path
import statistics
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"benchmarks"))
from report import aggregate, report


class BenchmarkAnalysis(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix="erpc-report-",dir="/tmp")
        self.addCleanup(self.temporary.cleanup)
        self.path=Path(self.temporary.name)
        config=dict(mode="socket-async",tensor_bytes=4096,inflight=1,workers=1,buffer_slots=0,workload="compute",
                    delay_us=0,prepare_delay_us=0,instrumented=1)
        self.metadata=dict(configurations=[config],trials=5,requests=2,warmup=1)
        self.manifest=dict(status="completed",runs=[])
        for trial in range(1,6):
            name=f"synthetic-{trial}"
            rows=[dict(config,run_id=name,request_id=index+2,status="0",latency_us=10+trial+10*index,
                       queue_us=1,execute_us=1,buffer_wait_us=0,slot=0,generation=0,
                       prepare_begin_ns=10000,prepare_end_ns=11000,queued_ns=12000,execute_begin_ns=13000,execute_end_ns=14000)
                  for index in range(2)]
            self.write_rows(name+".csv",rows)
            summary=dict(config,run_id=name,completed=2,failures=0,warmup=1,prepare_us=0,
                         measured_seconds=.02,throughput_rps=100,p50_us=10+trial,p95_us=20+trial,p99_us=20+trial,
                         initialization_us=5,first_request_us=30,
                         socket_tx_bytes=8248,socket_rx_bytes=8296,warmup_tx_bytes=4124,warmup_rx_bytes=4148,
                         initialization_tx_bytes=0,initialization_rx_bytes=0)
            self.manifest["runs"].append(dict(name=name,trial=trial,config=config,status="completed",returncode=0,
                                              samples=name+".csv",summary=summary))
        self.save()

    def write_rows(self,name,rows):
        with (self.path/name).open("w",newline="") as stream:
            writer=csv.DictWriter(stream,fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

    def save(self):
        (self.path/"metadata.json").write_text(json.dumps(self.metadata))
        (self.path/"manifest.json").write_text(json.dumps(self.manifest))

    def mutate_sample(self,key,value):
        path=self.path/self.manifest["runs"][0]["samples"]
        with path.open(newline="") as stream:
            rows=list(csv.DictReader(stream))
        rows[0][key]=value
        self.write_rows(path.name,rows)

    def test_per_trial_aggregation_and_variability(self):
        _,trials,summary=aggregate(self.path)
        self.assertEqual(len(trials),5)
        self.assertEqual(summary[0]["p50_us_median"],13)
        self.assertEqual(summary[0]["p99_us_median"],23)
        self.assertEqual(summary[0]["p99_us_min"],21)
        self.assertEqual(summary[0]["p99_us_max"],25)
        self.assertAlmostEqual(summary[0]["p99_us_stdev"],statistics.stdev([21,22,23,24,25]))
        report(self.path,plots=False)
        self.assertTrue((self.path/"summary.csv").exists())
        self.assertIn("Raw latencies are not pooled",(self.path/"report.md").read_text())

    def test_failed_or_incomplete_experiment_rejected(self):
        for status in ("failed","running"):
            self.manifest["status"]=status
            self.save()
            with self.assertRaises(ValueError):
                aggregate(self.path)

    def test_failed_trial_rejected(self):
        self.manifest["runs"][0]["status"]="failed"
        self.save()
        with self.assertRaises(ValueError): aggregate(self.path)

    def test_missing_repetition_rejected(self):
        self.manifest["runs"].pop()
        self.save()
        with self.assertRaises(ValueError): aggregate(self.path)

    def test_duplicate_repetition_rejected(self):
        self.manifest["runs"][-1]["trial"]=1
        self.save()
        with self.assertRaises(ValueError): aggregate(self.path)

    def test_reused_samples_cannot_impersonate_repetitions(self):
        self.manifest["runs"][-1]["samples"]=self.manifest["runs"][0]["samples"]
        self.manifest["runs"][-1]["summary"]=self.manifest["runs"][0]["summary"]
        self.save()
        with self.assertRaises(ValueError): aggregate(self.path)

    def test_counter_mismatch_rejected(self):
        self.manifest["runs"][0]["summary"]["socket_tx_bytes"]-=1
        self.save()
        with self.assertRaises(ValueError): aggregate(self.path)

    def test_summary_percentile_mismatch_rejected(self):
        self.manifest["runs"][0]["summary"]["p99_us"]+=1
        self.save()
        with self.assertRaises(ValueError): aggregate(self.path)

    def test_invalid_latency_rejected(self):
        for value in ("nan","inf","-1"):
            self.mutate_sample("latency_us",value)
            with self.assertRaises(ValueError): aggregate(self.path)

    def test_missing_or_duplicate_request_rejected(self):
        self.mutate_sample("request_id",3)
        with self.assertRaises(ValueError): aggregate(self.path)

    def test_configuration_mismatch_rejected(self):
        self.mutate_sample("inflight",4)
        with self.assertRaises(ValueError): aggregate(self.path)

    def test_invalid_stage_order_rejected(self):
        self.mutate_sample("execute_end_ns",12000)
        with self.assertRaises(ValueError): aggregate(self.path)

    def test_missing_samples_rejected(self):
        (self.path/self.manifest["runs"][0]["samples"]).unlink()
        with self.assertRaises(FileNotFoundError): aggregate(self.path)


if __name__=="__main__":
    unittest.main(verbosity=2)
