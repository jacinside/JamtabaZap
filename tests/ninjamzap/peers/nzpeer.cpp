// Real njclient sender with real H.264 in the spec's wire format.
// usage: nzpeer host port name framesfile spsfile fps [video:0|1] [seconds]
#include "TestClient.h"
#include "njclient.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>
using namespace videosync;
static std::vector<uint8_t> rd(const char *p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int c,char**v){
  TestClient::Options o; o.host=v[1]; o.port=atoi(v[2]); o.user=std::string("anonymous:")+v[3];
  bool video=c>7?atoi(v[7]):true; int secs=c>8?atoi(v[8]):120; int fps=atoi(v[6]);
  o.sendAudio=true; o.sendVideo=video; o.toneInput=true;
  TestClient t(o); if(!t.connectAndJoin(std::chrono::seconds(10))){fprintf(stderr,"connect failed\n");return 1;}
  auto all=rd(v[4]); auto sps=rd(v[5]); std::vector<std::pair<const uint8_t*,int>> fr;
  for(size_t i=0;i+4<=all.size();){int n=(all[i]<<24)|(all[i+1]<<16)|(all[i+2]<<8)|all[i+3];fr.push_back({&all[i+4],n});i+=4+n;}
  if(video) t.raw()->SetVideoSPSPPS(sps.data(),(int)sps.size());
  printf("connected bpm=%d bpi=%d frames=%zu video=%d\n",t.getBPM(),t.getBPI(),fr.size(),video);fflush(stdout);
  auto start=std::chrono::steady_clock::now(); auto next=start; int last=t.intervalSwapCount(); size_t idx=0; long sent=0;
  while(std::chrono::steady_clock::now()-start<std::chrono::seconds(secs)){
    int s=t.intervalSwapCount(); if(s!=last){last=s; idx=0; printf("swap %d sent=%ld\n",s,sent);fflush(stdout);}
    if(video){ if(idx<fr.size()){t.sendVideoFrame(fr[idx].first,fr[idx].second); idx++; sent++;} }
    next+=std::chrono::milliseconds(1000/fps); std::this_thread::sleep_until(next);
  }
  t.disconnect(); return 0;
}
