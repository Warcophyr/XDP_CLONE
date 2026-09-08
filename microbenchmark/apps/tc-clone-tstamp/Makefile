TARGET = tc_clone

all: $(TARGET)

$(TARGET): $(TARGET).c $(TARGET).bpf.c
	clang -g -O2 --target=bpf -c $(TARGET).bpf.c -o $(TARGET).bpf.o
	bpftool gen skeleton $(TARGET).bpf.o > $(TARGET).bpf.skel.h
	gcc -g -O2 -o $(TARGET) $(TARGET).c -lbpf

clean:
	rm -f $(TARGET) $(TARGET).bpf.o $(TARGET).bpf.skel.h