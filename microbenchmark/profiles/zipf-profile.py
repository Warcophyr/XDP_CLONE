"""
Profilo TRex STL per generare traffico con distribuzione Zipf.
Versione PPS: usa pps fissi invece di percentage (più stabile).
"""

import numpy as np
from trex.stl.api import *


class ZipfProfile(object):
    """Genera traffico UDP con distribuzione Zipf usando PPS fissi"""
    
    def __init__(self):
        self.num_flows = 10000
        self.skew = 0.6
        self.packet_size = 64
        
    def _generate_flow_weights(self):
        """Calcola i pesi Zipf per ogni flusso"""
        if self.skew < 1.0:
            ranks = np.arange(1, self.num_flows + 1)
            weights = 1.0 / np.power(ranks, self.skew)
            probs = weights / weights.sum()
        else:
            ranks = np.arange(1, self.num_flows + 1)
            weights = 1.0 / np.power(ranks, self.skew)
            probs = weights / weights.sum()
        return probs
    
    def _generate_ip_pools(self):
        """Genera pool di IP per i flussi"""
        PREFIXES = [
            ("10", 1), ("172", 16), ("192", 168),
            ("23", None), ("91", None),
        ]
        
        np.random.seed(42)
        src_ips = []
        dst_ips = []
        seen_src = set()
        seen_dst = set()
        
        while len(src_ips) < self.num_flows:
            p, fixed = PREFIXES[np.random.randint(0, len(PREFIXES))]
            if fixed is None:
                ip = f"{p}.{np.random.randint(0,256)}.{np.random.randint(0,256)}.{np.random.randint(1,255)}"
            else:
                ip = f"{p}.{fixed}.{np.random.randint(0,256)}.{np.random.randint(1,255)}"
            if ip not in seen_src:
                seen_src.add(ip)
                src_ips.append(ip)
        
        while len(dst_ips) < self.num_flows:
            p, fixed = PREFIXES[np.random.randint(0, len(PREFIXES))]
            if fixed is None:
                ip = f"{p}.{np.random.randint(0,256)}.{np.random.randint(0,256)}.{np.random.randint(1,255)}"
            else:
                ip = f"{p}.{fixed}.{np.random.randint(0,256)}.{np.random.randint(1,255)}"
            if ip not in seen_dst:
                seen_dst.add(ip)
                dst_ips.append(ip)
        
        return src_ips, dst_ips
    
    def get_streams(self, direction=0, **kwargs):
        """Crea stream TRex - PPS mode"""
        
        self.num_flows = kwargs.get('num_flows', 10000)
        self.skew = kwargs.get('skew', 0.6)
        self.packet_size = kwargs.get('packet_size', 64)
        
        print(f"\n[ZipfProfile PPS] Configurazione:")
        print(f"  • Numero flussi: {self.num_flows:,}")
        print(f"  • Skew Zipf: {self.skew}")
        
        if self.num_flows > 20000:
            print(f"  ⚠️  Riduco a 20000 flussi")
            self.num_flows = 20000
        
        src_ips, dst_ips = self._generate_ip_pools()
        probs = self._generate_flow_weights()
        
        np.random.seed(42)
        src_ports = np.random.randint(1024, 65536, size=self.num_flows)
        dst_ports = np.random.randint(1024, 65536, size=self.num_flows)
        
        streams = []
        payload_size = max(0, self.packet_size - 42)
        
        # PPS base molto basso (il rate totale viene controllato da NDRBench)
        BASE_PPS = 1.0
        
        # Info per statistiche
        top_flows_info = []
        
        for flow_idx in range(self.num_flows):
            src_ip = src_ips[flow_idx]
            dst_ip = dst_ips[flow_idx]
            src_port = int(src_ports[flow_idx])
            dst_port = int(dst_ports[flow_idx])
            flow_prob = probs[flow_idx]
            
            pkt = (
                Ether(src="e8:eb:d3:78:95:8d", dst="58:a2:e1:d0:69:ce") /
                IP(src=src_ip, dst=dst_ip) /
                UDP(sport=src_port, dport=dst_port) /
                Raw(load=b'\x42' * payload_size)
            )
            
            # PPS proporzionale alla probabilità Zipf
            flow_pps = BASE_PPS * flow_prob * self.num_flows
            
            stream = STLStream(
                packet=STLPktBuilder(pkt=pkt),
                mode=STLTXCont(pps=flow_pps)
            )
            
            streams.append(stream)
            
            # Salva info top 10 flows
            if flow_idx < 10:
                top_flows_info.append((flow_idx, src_ip, dst_ip, flow_prob))
        
        # Statistiche dettagliate (come zipf_profile.py)
        print(f"  ✅ Creati {len(streams):,} stream (PPS mode)")
        print(f"\n  📊 Top 10 flussi (distribuzione Zipf):")
        for idx, src_ip, dst_ip, prob in top_flows_info:
            print(f"     #{idx+1:2d}: {src_ip:15} → {dst_ip:15}  ({prob*100:.2f}%)")
        
        # Concentrazione traffico
        top_10_percent = sum(probs[:10]) * 100
        top_100_percent = sum(probs[:min(100, len(probs))]) * 100
        print(f"\n  📈 Concentrazione traffico:")
        print(f"     Top 10 flussi:  {top_10_percent:.1f}%")
        print(f"     Top 100 flussi: {top_100_percent:.1f}%")
        
        return streams


def register():
    return ZipfProfile()
