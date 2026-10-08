// Independent GPU-drawn recovery card, available even without a renderer.
// No borrowed CEF resources and no foreign recovery document/native bridge.
void RecoveryCard(const char* title,const char* action,bool confirmation=false) {
  const char* alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789- .";
  const uint64_t glyphs[]={
    0x1f111f110e,0x0f110f110f,0x0e1101010e,0x0f1111110f,0x1f011f011f,0x01011f011f,
    0x0e1119010e,0x11111f1111,0x0e0404040e,0x0611090808,0x1111090511,0x1f01010101,
    0x1111151b11,0x1111191511,0x0e1111110e,0x01010f110f,0x161915110e,0x11090f110f,
    0x0f100e011e,0x040404041f,0x0e11111111,0x040a111111,0x111b151111,0x110a040a11,
    0x0404040a11,0x1f0204081f,0x0e1315150e,0x0e04040604,0x1f020c110e,0x0e111c100f,
    0x10101f1212,0x0f101f011f,0x0e110f010e,0x020408101f,0x0e110e110e,0x0e101e110e,
    0x00001f0000,0,0x0400000000};
  int w,h;SDL_GetWindowSizeInPixels(window,&w,&h);glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,w,h);
  glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glClearColor(.025f,.045f,.085f,1);glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  auto text=[&](const char* str,int y,int scale){int len=strlen(str),x=std::max(12,(w-len*6*scale)/2);
    for(int i=0;str[i];++i){const char* found=strchr(alphabet,str[i]);uint64_t bits=found?glyphs[found-alphabet]:0;
      for(int row=0;row<5;++row)for(int col=0;col<5;++col)if((bits>>(row*8+col))&1){glScissor(x+i*6*scale+col*scale,h-y-(row+1)*scale,scale,scale);glClear(GL_COLOR_BUFFER_BIT);}}};
  glClearColor(.98f,.75f,.14f,1);text(title,h/2-55,std::max(2,std::min(4,w/150)));
  glClearColor(.85f,.9f,1,1);text(action,h/2+15,std::max(2,std::min(3,w/220)));
  text(confirmation?"ESC CANCEL - ENTER INSTALL":"ALT-F4 TO CLOSE",h/2+65,2);
  int lw,lh;SDL_GetWindowSize(window,&lw,&lh);
  auto button=[&](int x,int width){glClearColor(.18f,.24f,.33f,1);glScissor(x*w/lw,h-(lh/2+125)*h/lh,width*w/lw,40*h/lh);glClear(GL_COLOR_BUFFER_BIT);};
  if(confirmation){button(lw/2-250,240);button(lw/2+10,240);}else button(lw/2-130,260);
  glClearColor(.98f,.75f,.14f,1);text(confirmation?"CANCEL          CLOSE AND INSTALL":"RETRY NOW",(lh/2+98)*h/lh,2);glDisable(GL_SCISSOR_TEST);
}
