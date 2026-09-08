TARGET := xdp_clone
BPF_SRC := $(TARGET).bpf.c
BPF_OBJ := $(TARGET).bpf.o
SKEL := $(TARGET).bpf.skel.h
USER_SRC := $(TARGET).c

.PHONY : all clean


all: $(TARGET)


$(BPF_OBJ): $(BPF_SRC)
	clang -g -O2 --target=bpf -c $< -o $@

$(SKEL): $(BPF_OBJ)
	bpftool gen skeleton $< > $@

$(TARGET): $(USER_SRC) $(SKEL)
	gcc -g -O2 -o $@ $< -lbpf


clean:
	rm -f $(TARGET) $(BPF_OBJ) $(SKEL)
