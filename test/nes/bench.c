#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "nes_core.h"
static uint8_t *prg,*chr;
static void remap(nes_t*n){for(int i=0;i<8;i++){n->prg_ptr[i]=prg+n->prg_off[i];if(n->chr_off[i]!=NES_NOMAP)n->chr_ptr[i]=chr+n->chr_off[i];}}
static volatile int sink;
static void lcb(nes_t*n,int y,const uint8_t*idx){sink+=idx[y&255];}
int main(int c,char**v){FILE*f=fopen(v[1],"rb");uint8_t h[16];fread(h,1,16,f);static nes_t N;uint32_t tr;nes_load_header(&N,h,&tr);
prg=malloc(N.prg_size);fread(prg,1,N.prg_size,f);chr=malloc(N.chr_size);fread(chr,1,N.chr_size,f);N.remap=remap;N.line_cb=lcb;nes_cart_init(&N);
for(int d=0;d<2;d++){clock_t t=clock();for(int i=0;i<600;i++)nes_frame(&N,0,d);double s=(double)(clock()-t)/CLOCKS_PER_SEC;printf("draw=%d %.1f us/frame\n",d,s/600*1e6);}}
