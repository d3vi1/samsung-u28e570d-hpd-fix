import ghidra.app.script.GhidraScript;
import ghidra.app.plugin.assembler.*;
import java.nio.file.*;
import java.io.*;
public class BuildHpdStub extends GhidraScript {
 public void run() throws Exception {
  String dir=getScriptArgs()[0]; Assembler asm=Assemblers.getAssembler(currentProgram);
  ByteArrayOutputStream out=new ByteArrayOutputStream(); StringBuilder listing=new StringBuilder();
  String[] code={
   "b.movhi r7,0xa020", "b.ori r7,r7,0x303c", "b.lbz r7,0x0(r7)", "b.bnei r7,0x2,0x2eb600",
   "b.movhi r7,0x36", "b.lhz r7,-0x57d2(r7)", "b.bnei r7,0xa,0x2eb600",
   "b.movhi r7,0x36", "b.lhz r7,-0x57d0(r7)", "b.bnei r7,0x0,0x2eb600",
   "b.movhi r7,0x36", "b.lhz r7,-0x57ca(r7)", "b.bnei r7,0x0,0x2eb600",
   "b.lbz r7,0x844(r6)", "b.andi r7,r7,0x20", "b.bnei r7,0x0,0x2eb600",
   "b.lbz r7,0x840(r6)", "b.andi r7,r7,0x20", "b.beqi r7,0x0,0x2eb600",
   "b.j 0x240a2f"
  };
  long pc=0x2eb500;
  for(String line:code){byte[] b=asm.assembleLine(toAddr(pc),line);listing.append(String.format("%08x %s %s%n",pc,java.util.HexFormat.of().formatHex(b),line));out.write(b);pc+=b.length;}
  if(pc>0x2eb600)throw new Exception("stub overflow");
  while(pc<0x2eb600){out.write(0);pc++;}
  for(String line:new String[]{"b.lbz r7,0x840(r6)","b.j 0x240a28"}){byte[] b=asm.assembleLine(toAddr(pc),line);listing.append(String.format("%08x %s %s%n",pc,java.util.HexFormat.of().formatHex(b),line));out.write(b);pc+=b.length;}
  byte[] hook=asm.assembleLine(toAddr(0x240a24),"b.j 0x2eb500");
  if(hook.length!=4)throw new Exception("hook width not4");
  Files.write(Path.of(dir,"stub.bin"),out.toByteArray()); Files.write(Path.of(dir,"hook.bin"),hook);
  Files.writeString(Path.of(dir,"stub-assembly.txt"),listing.toString()); println("STUB BUILD OK");
 }
}
