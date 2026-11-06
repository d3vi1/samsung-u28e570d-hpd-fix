#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <unistd.h>
#include <errno.h>
typedef CFTypeRef AV;
int main(int argc,char **argv) { @autoreleasepool {
 uint64_t selected=0;
 if(argc!=1){
  if(argc!=3||strcmp(argv[1],"--registry-id")){fprintf(stderr,"Usage: read_firmware_id [--registry-id ID]\n");return 2;}
  char *end=NULL;errno=0;selected=strtoull(argv[2],&end,0);
  if(errno||!selected||!end||*end){fprintf(stderr,"Invalid registry ID.\n");return 2;}
 }
 CGDirectDisplayID ids[32]; uint32_t n=0; unsigned external=0;
 if(CGGetOnlineDisplayList(32,ids,&n)!=kCGErrorSuccess)return 2;
 for(uint32_t i=0;i<n;i++) if(!CGDisplayIsBuiltin(ids[i])){
  external++;
  if(CGDisplayIsAsleep(ids[i])||!CGDisplayIsActive(ids[i])){fprintf(stderr,"Refusing DDC while external display is asleep/inactive.\n");return 2;}
 }
 if(!external||(!selected&&external!=1)){fprintf(stderr,"Expected exactly one active external display, or an explicit registry ID.\n");return 2;}
 void *h=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW);
 if(!h)return 2;
 AV(*create)(CFAllocatorRef,io_service_t)=dlsym(h,"IOAVServiceCreateWithService");
 IOReturn(*wr)(AV,uint32_t,uint32_t,void*,uint32_t)=dlsym(h,"IOAVServiceWriteI2C");
 IOReturn(*rd)(AV,uint32_t,uint32_t,void*,uint32_t)=dlsym(h,"IOAVServiceReadI2C");
 if(!create||!wr||!rd){fprintf(stderr,"IOAV I2C functions unavailable.\n");dlclose(h);return 2;}
 io_iterator_t it=0; IOReturn e=IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("DCPAVServiceProxy"),&it);
 if(e){dlclose(h);return 2;}
 io_service_t service=0,s=0;unsigned count=0;
 while((s=IOIteratorNext(it))){uint64_t id=0;IORegistryEntryGetRegistryEntryID(s,&id);if(!selected||id==selected){count++;if(!service)service=s;else IOObjectRelease(s);}else IOObjectRelease(s);}
 IOObjectRelease(it);
 if(count!=1){if(service)IOObjectRelease(service);fprintf(stderr,"Expected exactly one DCPAVServiceProxy.\n");dlclose(h);return 2;}
 uint64_t rid=0;IORegistryEntryGetRegistryEntryID(service,&rid);
 AV av=create(kCFAllocatorDefault,service);IOObjectRelease(service);
 if(!av){dlclose(h);return 2;}
 // Fixed C3/0 identification query. No Set VCP or configuration command exists here.
 uint8_t q[]={0x82,0xc3,0x00,0x7e};
 e=wr(av,0x37,0x51,q,sizeof(q));
 if(e){fprintf(stderr,"Request failed: 0x%x\n",e);CFRelease(av);dlclose(h);return 1;}
 usleep(100000);
 uint8_t b[24]={0};e=rd(av,0x37,0x51,b,sizeof(b));
 printf("registry_id=0x%llx read_status=0x%x frame=",(unsigned long long)rid,e);
 for(int i=0;i<23;i++)printf("%02X%s",b[i],i==22?"\n":" ");
 uint8_t checksum=0x50;for(int i=0;i<23;i++)checksum^=b[i];
 int valid=!e&&b[0]==0x6e&&b[1]==0x94&&b[2]==0xc3&&b[3]==0&&checksum==0&&memcmp(b+4,"m-E5728UMDB-",12)==0;
 if(valid)printf("firmware=%.*s checksum=valid\n",18,b+4);else fprintf(stderr,"Invalid identification reply.\n");
 CFRelease(av);dlclose(h);return valid?0:1;
} }
