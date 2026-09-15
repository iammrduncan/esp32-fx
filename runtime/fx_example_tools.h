// SPDX-License-Identifier: MIT
#ifndef FX_EXAMPLE_TOOLS_H
#define FX_EXAMPLE_TOOLS_H
#include <stdint.h>
int32_t fx_example_tool_call(const uint8_t *name, uint32_t name_len,
                            const uint8_t *arguments, uint32_t arguments_len,
                            uint8_t *output, uint32_t output_cap, uint8_t *status);
#endif
