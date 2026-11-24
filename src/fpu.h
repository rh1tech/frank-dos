#ifndef FPU_H
#define FPU_H

#include <stdbool.h>
#include <stdint.h>

typedef struct FPU FPU;

FPU *fpu_new();
unsigned long fpu_state_size(void);
void fpu_delete(FPU *fpu);
bool fpu_exec1(FPU *fpu, void *cpu, int op, int group, unsigned int i);
bool fpu_exec2(FPU *fpu, void *cpu, bool opsz16, int op, int group, int seg, uint32_t addr);

#endif /* FPU_H */

/* frank-react HLE helpers */
void fpu_hle_push(FPU *fpu, double val);
double fpu_hle_pop(FPU *fpu);
uint16_t fpu_hle_get_cw(FPU *fpu);
void fpu_hle_set_cw(FPU *fpu, uint16_t cw);
void fpu_hle_reset(FPU *fpu);
