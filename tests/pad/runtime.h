/* tests/pad: the little of the runtime pad.c uses */
#pragma once
#include <stdint.h>
#include <stdio.h>
struct CPU;
#define LOG_ALWAYS 1
#define LOG_INPUT 2
#define LOG(cat, ...) do { (void)(cat); if (getenv("PAD_TEST_LOG")) { printf(__VA_ARGS__); printf("\n"); } } while (0)
extern int deterministic;
extern unsigned long frame_count;
const char *port_getenv(const char *name);
