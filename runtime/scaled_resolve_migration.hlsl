
cbuffer Parameters:register(b0) {
 uint sx,sy,dx,dy,bpp,dwords,row_stride,layout,tile_base,samples_x,samples_y;
};
ByteAddressBuffer old_data:register(t0);
RWByteAddressBuffer new_data:register(u0);
[numthreads(64,1,1)]
void main(uint3 tid:SV_DispatchThreadID) {
 uint index=tid.x+tid.y*row_stride;
 if(index>=dwords)return;
 if(layout!=0) {
   // A native tile is 80 32-bit words by 16 sample rows. At 64 bpp
   // each pixel occupies two adjacent words; resample that pair together.
   uint tile_words=1280*dx*dy;
   uint tile=index/tile_words,local=index%tile_words;
   uint words_per_pixel=(layout==2 || layout==4)?2:1;
   uint x_word=local%(80*dx),y=local/(80*dx);
   uint x_pixel=x_word/words_per_pixel;
   uint source_x=((2*(x_pixel/samples_x)+1)*sx/(2*dx))*samples_x+x_pixel%samples_x;
   uint source_y=((2*(y/samples_y)+1)*sy/(2*dy))*samples_y+y%samples_y;
   uint address=(tile*1280*sx*sy+source_y*80*sx+
                 source_x*words_per_pixel+x_word%words_per_pixel)*4;
   uint output_index=index;
   if(layout>=3) {
     uint pitch_tiles=16*words_per_pixel;
     uint output_tile=tile+tile_base;
     uint output_y=(output_tile/pitch_tiles)*(16*dy)+y;
     output_index=output_y*(1280*words_per_pixel*dx)+
                  (output_tile%pitch_tiles)*(80*dx)+x_word;
   }
   new_data.Store(output_index*4,old_data.Load(address));return;
 }
 uint unit=bpp==1?8:16;
 uint words=unit/4;
 uint group=index/(words*dx*dy);
 uint local=index%(words*dx*dy);
 uint phase=local/words,word=local%words;
 uint px=phase/dy,py=phase%dy;
 uint lanes=unit/bpp;
 uint value=0;
 [unroll]for(uint byte_index=0;byte_index<4;++byte_index) {
   uint byte_in_group=word*4+byte_index;
   uint dest_pixel=px*lanes+byte_in_group/bpp;
   uint source_pixel=(2*dest_pixel+1)*sx/(2*dx);
   uint source_phase=source_pixel/lanes*sy+(2*py+1)*sy/(2*dy);
   uint address=(group*sx*sy+source_phase)*unit+
                (source_pixel%lanes)*bpp+byte_in_group%bpp;
   uint component=(old_data.Load(address&~3u)>>((address&3)*8))&255;
   value|=component<<(byte_index*8);
 }
 new_data.Store(index*4,value);
}