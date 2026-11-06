// Real receiver core: records its audio output and, per delivered image frame, the output sample
// at delivery. usage: nzsync host port seconds outdir sender-prefix [stall-at-s stall-ms]
#include "TestClient.h"
#include "SyncLogCapture.h"
#include "njclient.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
using namespace videosync;
int main(int c,char**v){
  TestClient::Options op; op.host=v[1]; op.port=atoi(v[2]); op.user="anonymous:nzsync"; op.sendAudio=false; op.recordOutput=true;
  int secs=atoi(v[3]); std::string dir=v[4]; int stallAt=c>6?atoi(v[6]):0, stallMs=c>7?atoi(v[7]):0; bool stalled=false; auto&log=SyncLogCapture::instance(); log.clear();
  TestClient t(op); if(!t.connectAndJoin(std::chrono::seconds(10))) return 1;
  std::ofstream h264(dir+"/v.h264",std::ios::binary); std::ofstream idx(dir+"/frames.csv");
  auto start=std::chrono::steady_clock::now(); long img=0,all=0; static const char sc[4]={0,0,0,1};
  while(std::chrono::steady_clock::now()-start<std::chrono::seconds(secs)){
    if(stallMs&&!stalled&&std::chrono::steady_clock::now()-start>std::chrono::seconds(stallAt)){ t.stallAudio(stallMs); stalled=true; }
    for(auto&f:t.drainVideoFrames()){ if(f.username.rfind(v[5],0)!=0||f.data.empty()) continue; all++; auto&d=f.data;
      if(d.size()==20) continue;
      if(d.size()>4&&d[0]==0&&(d[2]&0x1f)==7){ size_t sl=(d[0]<<8)|d[1]; h264.write(sc,4); h264.write((char*)&d[2],sl); size_t pl=(d[2+sl]<<8)|d[3+sl]; h264.write(sc,4); h264.write((char*)&d[4+sl],pl); continue; }
      for(size_t i=0;i+4<=d.size();){ uint32_t l=(d[i]<<24)|(d[i+1]<<16)|(d[i+2]<<8)|d[i+3]; if(!l||i+4+l>d.size())break; h264.write(sc,4); h264.write((char*)&d[i+4],l); i+=4+l; }
      idx<<img++<<","<<f.outputSample<<","<<f.receivedAtSwap<<"\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::vector<float> L,R; t.outputSnapshot(L,R); std::ofstream a(dir+"/out.f32",std::ios::binary); a.write((char*)L.data(),L.size()*sizeof(float));
  printf("delivered=%ld image=%ld audioSamples=%zu\n",all,img,L.size());
  std::ofstream lg(dir+"/synclog.txt"); for(const auto&l:log.all()) lg<<l<<"\n";
  t.disconnect(); return 0;
}
