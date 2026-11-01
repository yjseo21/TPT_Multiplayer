// Standalone fallback check for builds without libwebp (no optional dependency).
#include "multiplayer/Canvas.h"
#include <iostream>
#include <stdexcept>
using namespace multiplayer;
int main()
{
    if (SupportsWebP()) throw std::runtime_error("this test must build without WebP");
    std::string raw(CanvasBytes,'\0');raw[100]=char(0xf3);
    CanvasFrame f{1,23,7,0,true,PackCanvas(raw)};
    auto packet=EncodeCanvas(f,1,nullptr,true);
    if(packet[4]!=0 || DecodeCanvas(packet).frame.pixels->planes!=raw)
        throw std::runtime_error("lossless fallback failed");
    bool rejected=false;
    try {PackCanvas(raw,30,2);}catch(const std::exception&) {rejected=true;}
    if(!rejected) throw std::runtime_error("unsupported WebP accepted");
    std::cout<<"Lossless no-WebP compilation and fallback PASS\n";
}
