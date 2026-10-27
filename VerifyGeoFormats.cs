using System;
using System.IO;
public static class GeoFormatCheck {
    static void Need(bool value,string message) { if (!value) throw new Exception(message); }
    public static void Run(string root) { Run(root,"20_18",true); }
    public static void Run(string root,string stem,bool fixture) {
        var files=new[]{"L2J/"+stem+".l2j","L2OFF/"+stem+"_conv.dat"};
        var readers=new BinaryReader[2];
        try {
            for(int f=0;f<2;f++) readers[f]=new BinaryReader(File.OpenRead(Path.Combine(root,files[f])));
            var off=readers[1];
            Need(off.ReadByte()==int.Parse(stem.Split('_')[0]) && off.ReadByte()==int.Parse(stem.Split('_')[1]) && off.ReadUInt16()==128 && off.ReadUInt16()==16,"PTS header");
            uint headerCells=off.ReadUInt32(),headerSimple=off.ReadUInt32(),headerFlat=off.ReadUInt32();
            uint cells=0,simple=0,flat=0;
            for(int b=0;b<65536;b++) {
                var types=new int[2]; var heights=new int[2]; var counts=new int[2];
                int offMarker=0,offLayers=0;
                for(int f=0;f<2;f++) {
                    int t=f==1 ? readers[f].ReadUInt16() : readers[f].ReadByte();
                    if(f==1) { offMarker=t; t=t==0 ? 0 : t==64 ? 1 : 2; }
                    Need(t>=0 && t<=2,"Invalid block marker"); types[f]=t;
                    if(t==0) {
                        heights[f]=readers[f].ReadInt16();
                        if(f==1) Need(heights[f]==readers[f].ReadInt16(),"PTS flat bounds");
                    }
                }
                Need(types[0]==types[1],"L2J/PTS block mismatch");
                if(fixture && b==771) for(int f=0;f<2;f++) Need(types[f]==0 && heights[f]==8,"Offset flat floor height");
                if(types[1]==0) { flat++; simple++; } else if(types[1]==1) simple++;
                for(int c=0;c<64;c++) {
                    for(int f=0;f<2;f++) counts[f]=types[f]==2 ? (f==1 ? readers[f].ReadUInt16() : readers[f].ReadByte()) : 1;
                    Need(counts[0]==counts[1] && counts[0]>0 && counts[0]<=125,"Layer counts differ");
                    if(types[1]!=0) { cells+=(uint)counts[1]; offLayers+=counts[1]; }
                    for(int l=0;l<counts[0];l++) {
                        int baseZ=0,baseFlags=0;
                        for(int f=0;f<2;f++) {
                            int z=heights[f],flags=15;
                            if(types[f]!=0) {
                                int data=readers[f].ReadInt16(); z=(data & -16)>>1; flags=data&15;
                            }
                            if(f==0) { baseZ=z; baseFlags=flags; }
                            else Need(z==baseZ && (flags&15)==baseFlags,"Height or cardinal movement differs between formats");
                            if(fixture && b==0 && c==18) Need(counts[0]==1 && z==152,"Roof height or sealed floor survived");
                            if(fixture && b==0 && c==26 && l==0) Need((flags&1)!=0,"Upper floor blocked");
                        }
                    }
                }
                if(types[1]==2) Need(offMarker==offLayers,"PTS block layer sum");
            }
            Need(cells==headerCells && simple==headerSimple && flat==headerFlat && (!fixture || flat>0),"PTS header counts and flat coverage");
            foreach(var r in readers) Need(r.BaseStream.Position==r.BaseStream.Length,"Trailing bytes");
        } finally { foreach(var r in readers) if(r!=null) r.Dispose(); }
    }
}
