// Copyright (c) 2026 Javier Cordero
// SPDX-License-Identifier: GPL-3.0-or-later
// Real njclient sender emitting Jamtaba's wire format: JTBv, raw Annex-B in >=4 KB writes, channel 1, flags 0.
// usage: nzjtbv host port name stream.h264 fps seconds [fourcc]
#include "TestClient.h"
#include "njclient.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>
using namespace videosync;
int main(int c,char**v){
  TestClient::Options o; o.host=v[1]; o.port=atoi(v[2]); o.user=std::string("anonymous:")+v[3]; o.sendAudio=true; o.toneInput=true;
  int fps=atoi(v[5]), secs=atoi(v[6]); const char*fc=c>7?v[7]:"JTBv";
  std::ifstream f(v[4],std::ios::binary); std::vector<uint8_t> d{std::istreambuf_iterator<char>(f),{}};
  std::vector<size_t> au; // access units begin at the SPS or at a slice NAL not preceded by SPS/PPS/SEI
  bool hdr=false; for(size_t i=0;i+4<d.size();i++){ if(d[i]||d[i+1]||d[i+2]!=1) continue; size_t st=(i>0&&d[i-1]==0)?i-1:i; int t=d[i+3]&31;
    if(t==7||t==8||t==6){ if(!hdr){au.push_back(st);hdr=true;} } else if(t==1||t==5){ if(!hdr) au.push_back(st); hdr=false; } }
  TestClient t(o); if(!t.connectAndJoin(std::chrono::seconds(10))) return 1;
  auto*n=t.raw(); n->SetLocalChannelInfo(1,"video",false,0,false,0,true,false,false,0,true,0); n->NotifyServerOfChannelChange();
  unsigned fcc=fc[0]|(fc[1]<<8)|(fc[2]<<16)|((unsigned)fc[3]<<24);
  printf("connected bpm=%d bpi=%d aus=%zu fourcc=%s\n",t.getBPM(),t.getBPI(),au.size(),fc);fflush(stdout);
  unsigned char g[16]; bool open=false; int last=t.intervalSwapCount(); size_t idx=0; std::vector<uint8_t> pend; long bytes=0;
  auto start=std::chrono::steady_clock::now(),next=start;
  while(std::chrono::steady_clock::now()-start<std::chrono::seconds(secs)){
    int s=t.intervalSwapCount();
    if(s!=last){ last=s; if(open){ n->RawDataSendWrite(g,pend.data(),(int)pend.size(),true); pend.clear(); } n->RawDataSendBegin(g,fcc,1,0); open=true; idx=0; printf("swap %d bytes=%ld\n",s,bytes);fflush(stdout);}    
    if(open&&idx<au.size()){ size_t a=au[idx],b=idx+1<au.size()?au[idx+1]:d.size(); pend.insert(pend.end(),d.begin()+a,d.begin()+b); idx++;
      if(pend.size()>=4096){ n->RawDataSendWrite(g,pend.data(),(int)pend.size(),false); bytes+=pend.size(); pend.clear(); } }
    next+=std::chrono::milliseconds(1000/fps); std::this_thread::sleep_until(next);
  }
  t.disconnect(); return 0;
}
