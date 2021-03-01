#!/bin/sh

# Write the value 0 = 0x00000000 to 0x1A4
# This will set the first four bits for prefetch disable to 0
sudo wrmsr --all 0x1A4 0

sudo rdmsr --all --hexadecimal 0x1A4 