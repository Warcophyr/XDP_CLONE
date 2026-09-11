import os

from trex_stl_lib.api import *

# Latency is measured on a probe stream that is never cloned, and the fan-out
# load runs alongside it on its own port. Measuring it on the cloned stream
# instead -- which is what this profile used to do -- reports how the generator
# absorbs a burst of n+1 frames rather than what the machine under test costs:
# the number steps by ~30us at three copies and then stops growing, identically
# for the XDP, the inline and the TC application, and it does so even at 100
# packets per second, where nothing anywhere is loaded.
#
# The applications route the two ports apart: 8901 is cloned, 8902 is bounced
# back untouched. See apps/xdp-clone-tstamp.
PORT_LOAD = 8901
PORT_PROBE = 8902

# Rates come from the environment so that lat.py owns them; see
# bench_common.LATENCY_LOAD_PPS / LATENCY_PROBE_PPS.
LOAD_PPS = int(os.environ.get("CLONLAT_LOAD_PPS", 100000))
PROBE_PPS = int(os.environ.get("CLONLAT_PROBE_PPS", 1000))

# pg_id 1 carries the latency histogram, pg_id 2 only counts, and its
# rx/tx ratio is what says whether the fan-out really happened.
PG_PROBE = 1
PG_LOAD = 2


class STLS1(object):
    def _pkt(self, dport):
        return (
            Ether(dst="00:00:00:00:00:01")
            / IP(src="20.0.0.1", dst="4.3.2.1")
            / UDP(sport=1234, dport=dport)
            / (b"\x00" * 10)
        )

    def create_streams(self):
        streams = [
            STLStream(
                packet=STLPktBuilder(pkt=self._pkt(PORT_PROBE)),
                mode=STLTXCont(pps=PROBE_PPS),
                flow_stats=STLFlowLatencyStats(pg_id=PG_PROBE),
            )
        ]
        if LOAD_PPS > 0:
            streams.append(
                STLStream(
                    packet=STLPktBuilder(pkt=self._pkt(PORT_LOAD)),
                    mode=STLTXCont(pps=LOAD_PPS),
                    flow_stats=STLFlowStats(pg_id=PG_LOAD),
                )
            )
        return streams

    def get_streams(self, tunables, **kwargs):
        return self.create_streams()


def register():
    return STLS1()
