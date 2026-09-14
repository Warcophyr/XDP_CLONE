# The MPI broadcast offload: a PMPI wrapper, a TC duplication point and an XDP
# one. Build on the machine that runs the ranks; the fan-out pieces are for the
# DUT, which needs the patched driver.

CLANG ?= clang
CC := gcc
MPICC ?= mpicc

LIBBPF_CFLAGS := $(shell pkg-config --cflags libbpf)
LIBBPF_LDFLAGS := $(shell pkg-config --libs libbpf)

BPF_CFLAGS := -g -O2 -target bpf -Ibpf $(LIBBPF_CFLAGS) -Wall \
              -Wno-address-of-packed-member -Wno-compare-distinct-pointer-types

.PHONY: all clean wrapper bpf

all: wrapper bpf

wrapper: libmpiclone.so

libmpiclone.so: src/bcast_wrapper.c src/wire.h
	$(MPICC) -O2 -Wall -Isrc -fPIC -shared -o $@ $<

bpf: bpf/mpi_fanout.bpf.o bpf/mpi_fanout_inline.bpf.o bpf/mpi_tc.bpf.o \
     bpf/mpi_fanout bpf/mpi_tc

bpf/mpi_fanout.bpf.o: bpf/mpi_fanout.bpf.c bpf/fanout_common.h src/wire.h bpf/axdp_tx.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

bpf/mpi_fanout_inline.bpf.o: bpf/mpi_fanout.bpf.c bpf/fanout_common.h src/wire.h bpf/axdp_tx.h
	$(CLANG) $(BPF_CFLAGS) -DAXDP_INLINE -c $< -o $@

bpf/mpi_tc.bpf.o: bpf/mpi_tc.bpf.c bpf/fanout_common.h src/wire.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

bpf/mpi_fanout: bpf/mpi_fanout.c bpf/fanout_common.h src/wire.h
	$(CC) -O2 -Wall -Ibpf $(LIBBPF_CFLAGS) -o $@ $< $(LIBBPF_LDFLAGS)

bpf/mpi_tc: bpf/mpi_tc.c bpf/fanout_common.h src/wire.h
	$(CC) -O2 -Wall -Ibpf $(LIBBPF_CFLAGS) -o $@ $< $(LIBBPF_LDFLAGS)

clean:
	rm -f libmpiclone.so bpf/*.bpf.o bpf/mpi_fanout bpf/mpi_tc
