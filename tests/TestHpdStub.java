import ghidra.app.script.GhidraScript;
import ghidra.app.emulator.EmulatorHelper;
import java.nio.file.*;
import java.util.*;
public class TestHpdStub extends GhidraScript {
 void put16(EmulatorHelper e,long a,int v){e.writeMemory(toAddr(a),new byte[]{(byte)v,(byte)(v>>8)});}
 int get8(EmulatorHelper e,long a){return e.readMemory(toAddr(a),1)[0]&255;}
 public void run() throws Exception {
  String dir=getScriptArgs()[0];byte[] stub=Files.readAllBytes(Path.of(dir,"stub.bin")),hook=Files.readAllBytes(Path.of(dir,"hook.bin"));
  int count=0,preserved=0; Random rand=new Random(1005);
  for(int marker:new int[]{0,1,2,255})for(int reason:new int[]{0,10,12,14,266})for(int port:new int[]{0,1,256})for(int off:new int[]{0,1,256})for(int pins=0;pins<4;pins++){
   EmulatorHelper e=new EmulatorHelper(currentProgram);
   try {
    e.writeMemory(toAddr(0x240a24),hook);e.writeMemory(toAddr(0x2eb500),stub);
    e.writeMemory(toAddr(0x33af8c),new byte[]{0,0,0,(byte)0xa0});
    e.writeMemory(toAddr(0xa020303cL),new byte[]{(byte)marker});
    put16(e,0x35a82e,reason);put16(e,0x35a830,port);put16(e,0x35a836,off);
    int out=(rand.nextInt(256)&0xdf)|((pins&1)<<5),oen=(rand.nextInt(256)&0xdf)|((pins&2)<<4);
    e.writeMemory(toAddr(0xa0000840L),new byte[]{(byte)out});e.writeMemory(toAddr(0xa0000844L),new byte[]{(byte)oen});
    long[] regs=new long[32];for(int r=0;r<32;r++){regs[r]=rand.nextInt()&0xffffffffL;e.writeRegister("r"+r,regs[r]);}
    int flag=count&1,carry=(count>>1)&1;e.writeRegister("F",flag);e.writeRegister("CY",carry);e.writeRegister("pc",0x240a1c);
    int steps=0,stores=0;while(e.getExecutionAddress().getOffset()!=0x240a3e){
     if(e.getExecutionAddress().getOffset()==0x240a2b)stores++;
     if(++steps>100||!e.step(monitor))throw new Exception("EMU stopped "+e.getExecutionAddress()+" "+e.getLastError());
    }
    boolean keep=marker==2&&reason==10&&port==0&&off==0&&(out&32)!=0&&(oen&32)==0;
    if(get8(e,0xa0000840L)!=(keep?out:out&0xdf)||get8(e,0xa0000844L)!=(oen&0xdf)||stores!=(keep?0:1))throw new Exception("GPIO or store mismatch case "+count);
    for(int r=1;r<32;r++){long expect=r==5?0x340000:r==6?0xa0000000L:r==7?(oen&0xdf):regs[r];if((e.readRegister("r"+r).longValue()&0xffffffffL)!=expect)throw new Exception("Register clobber r"+r+" case "+count);}
    if(e.readRegister("F").intValue()!=flag||e.readRegister("CY").intValue()!=carry)throw new Exception("Flags clobbered case "+count);
    if(keep)preserved++;count++;
   }finally{e.dispose();}
  }
  String result="PASS: "+count+" emulated state combinations; "+preserved+" preserve path; all fallback paths, full-word checks, unrelated GPIO bits, registers and flags verified. This is CPU p-code emulation, not AUX or hardware qualification.\n";
  Files.writeString(Path.of(dir,"emulation-result.txt"),result);println(result);
 }
}
