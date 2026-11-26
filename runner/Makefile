CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -fPIC -fno-strict-aliasing
GAME = ../user_data/bedrock/versions/1.21.70.03/lib/x86_64/libminecraftpe.so

all: runner libsupcompat.so

src/gl_bridge.S src/gl_slots.c: src/gen_gl.py $(GAME)
	python3 src/gen_gl.py $(GAME) src/gl_bridge.S src/gl_slots.c

libsupcompat.so: src/compat.c src/ndk.c src/jni_runtime.c src/gl_bridge.S src/gl_slots.c
	$(CC) $(CFLAGS) -shared -o $@ src/compat.c src/ndk.c src/jni_runtime.c src/gl_bridge.S src/gl_slots.c -ldl -Wl,--no-as-needed -lunwind-x86_64 -lunwind

runner: src/loader.c
	$(CC) $(CFLAGS) -o $@ src/loader.c -ldl -lpthread -lm -lstdc++

run: runner libsupcompat.so
	LD_LIBRARY_PATH=. ./runner $(GAME)

run-skip: runner libsupcompat.so
	LD_LIBRARY_PATH=. ./runner --skip-init $(GAME)

run-libs: runner libsupcompat.so
	LD_LIBRARY_PATH=. ./runner --lib ../user_data/bedrock/versions/1.21.70.03/lib/x86_64/libc++_shared.so --lib ../user_data/bedrock/versions/1.21.70.03/lib/x86_64/libfmod.so --lib ../user_data/bedrock/versions/1.21.70.03/lib/x86_64/libMediaDecoders_Android.so $(GAME)

run-libs-skip: runner libsupcompat.so
	LD_LIBRARY_PATH=. ./runner --skip-init --lib ../user_data/bedrock/versions/1.21.70.03/lib/x86_64/libc++_shared.so --lib ../user_data/bedrock/versions/1.21.70.03/lib/x86_64/libfmod.so --lib ../user_data/bedrock/versions/1.21.70.03/lib/x86_64/libMediaDecoders_Android.so $(GAME)

clean:
	rm -f runner libsupcompat.so src/gl_bridge.S src/gl_slots.c

.PHONY: all run run-skip run-libs run-libs-skip clean