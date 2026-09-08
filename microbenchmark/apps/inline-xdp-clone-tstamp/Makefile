TARGET := inline_xdp_clone
BPF_SRC := $(TARGET).bpf.c
BPF_OBJ := $(TARGET).bpf.o
SKEL := $(TARGET).bpf.skel.h
USER_SRC := $(TARGET).c

# axdp_tx.h -- the BPF side of the A-XDP TX descriptor ABI -- lives with the
# driver example that documents it, so there is one copy of it in the tree.
AXDP_INCLUDE := ../../../mellanox-clone-xdp/examples/inline-clone

.PHONY : all clean


all: $(TARGET)


$(BPF_OBJ): $(BPF_SRC) $(AXDP_INCLUDE)/axdp_tx.h
	clang -g -O2 --target=bpf -I $(AXDP_INCLUDE) -c $< -o $@

$(SKEL): $(BPF_OBJ)
	bpftool gen skeleton $< > $@

$(TARGET): $(USER_SRC) $(SKEL)
	gcc -g -O2 -o $@ $< -lbpf


clean:
	rm -f $(TARGET) $(BPF_OBJ) $(SKEL)
