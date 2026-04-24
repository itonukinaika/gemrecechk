CC      = clang
CFLAGS  = -O2 -Wall -std=c11 -DUNICODE -D_UNICODE -DEZXML_NOMMAP -I.
LDFLAGS = -mwindows -municode -lwinhttp -lcomctl32 -lgdi32 -luser32 -lkernel32 -lcomdlg32 -lcrypt32 -lcryptui -static -s

TARGET  = gemrecechk.exe
OBJS    = main.o orca_api.o gemini_api.o print.o cJSON.o ezxml.o res.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

res.o: gemrecechk.rc resource.h
	windres -c 65001 gemrecechk.rc -o res.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)