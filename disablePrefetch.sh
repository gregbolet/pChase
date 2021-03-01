#!/bin/sh

# Write the value 15 = 0x0000000F to 0x1A4
# This will set the first four bits for prefetch disable to 1
sudo wrmsr --all 0x1A4 15

sudo rdmsr --all --hexadecimal 0x1A4 