include $(HCLIB_ROOT)/include/hclib.mak

TARGETS=boot0 async0 async1 finish0 finish1 finish2  forasync1DCh  forasync1DRec \
		forasync2DCh  forasync2DRec  forasync3DCh  forasync3DRec deadlock0 \
		promise/asyncAwait0 promise/asyncAwait0Null promise/asyncAwait1 promise/future0 \
		promise/future1 promise/future2 promise/future3

FLAGS=-g

all: $(TARGETS) targets.txt

.PHONY: targets.txt # always update
targets.txt:
	@echo "$(TARGETS)" > $@

%: %.c
	$(CC) ${FLAGS} $(PROJECT_CFLAGS) $(PROJECT_LDFLAGS) -o $@ $^ $(PROJECT_LDLIBS)

clean:
	rm -f $(TARGETS)
