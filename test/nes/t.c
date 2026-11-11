#include <stdio.h>
#include <stdlib.h>
#include "nes_core.h"
static uint8_t *prg, *chr;
static void remap(nes_t *n){
  for(int i=0;i<8;i++){ n->prg_ptr[i]=prg+n->prg_off[i]; if(n->chr_off[i]!=NES_NOMAP) n->chr_ptr[i]=chr+n->chr_off[i]; }
}
static uint8_t fb[240][256];
static void lcb(nes_t*n,int y,const uint8_t*idx){ memcpy(fb[y],idx,256); }
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); uint8_t h[16]; fread(h,1,16,f);
  static nes_t N; uint32_t tr; int r=nes_load_header(&N,h,&tr); if(r){printf("hdr %d\n",r);return 1;}
  prg=malloc(N.prg_size); fread(prg,1,N.prg_size,f); chr=malloc(N.chr_size?N.chr_size:1); fread(chr,1,N.chr_size,f);
  N.remap=remap; N.line_cb=lcb; nes_cart_init(&N);
  int frames=atoi(argv[2]);
  for(int i=0;i<frames;i++) nes_frame(&N,0,i==frames-1);
  printf("pc=%04x a=%02x x=%02x y=%02x ram0..7:",N.pc,N.a,N.x,N.y); for(int i=0;i<8;i++)printf(" %02x",N.ram[i]); printf("\n");
  FILE*o=fopen(argv[3],"wb"); fprintf(o,"P6 256 240 255\n");
  static const uint8_t pl[16][3]={{84,84,84},{0,30,116},{8,16,144},{48,0,136},{68,0,100},{92,0,48},{84,4,0},{60,24,0},{32,42,0},{8,58,0},{0,64,0},{0,60,0},{0,50,60},{0,0,0},{0,0,0},{0,0,0}};
  for(int y=0;y<240;y++)for(int x=0;x<256;x++){int c=fb[y][x]; uint8_t rgb[3]; int g=(c&15)*16+(c>>4)*4; rgb[0]=(c&15)*17; rgb[1]=(c>>4)*60; rgb[2]=(c&15)*8+(c>>4)*20; fwrite(rgb,1,3,o);} 
  return 0;}
