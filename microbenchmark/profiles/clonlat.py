from trex_stl_lib.api import *

class STLS1(object):
    def _build_base_pkt(self):
        return (
            Ether(dst="00:00:00:00:00:01")
            / IP(src="20.0.0.1", dst="4.3.2.1")
            / UDP(sport=1234, dport=8901)
            / (b"\x00" * 10)
        )

    def create_streams(self):
        base_pkt = self._build_base_pkt()

        lat_stream = STLStream(
            packet=STLPktBuilder(pkt=base_pkt),
            mode=STLTXCont(pps=1000),
            # mode=STLTXCont(),
            flow_stats=STLFlowLatencyStats(pg_id=1),
        )

        return [lat_stream]

    def get_streams(self, tunables, **kwargs):
        return self.create_streams()


def register():
    return STLS1()