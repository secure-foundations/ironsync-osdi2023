using Impl_Compile;

using System;
using System.IO;

namespace Impl_Compile {
  public partial class DiskIOHandler {
    const int BLOCK_SIZE = 8*1024*1024;

    public void write(ulong lba, byte[] sector) {
      if (sector.Length != BLOCK_SIZE) {
        // We should never get here due to the contract.
        throw new Exception("Block must be exactly BLOCK_SIZE bytes");
      }

      File.WriteAllBytes(getFilename(lba), sector);
    }

    public void read(ulong lba, out byte[] sector) {
      string filename = getFilename(lba);
      byte[] bytes = File.ReadAllBytes(filename);
      if (bytes.Length != BLOCK_SIZE) {
        throw new Exception("Invalid block at " + filename);
      }
      sector = bytes;
    }

    private string getFilename(ulong lba) {
      return ".veribetrfs-storage/" + lba.ToString("X16");
    }
  }
}

class Application {
  // TODO hard-coding these types is annoying... is there another option?
  public BetreeGraphAsyncBlockCache_Compile.Constants k;
  public ImplState_Compile.ImplHeapState hs;

  public DiskIOHandler io;

  public Application() {
    initialize();
    verbose = true;
  }

  public bool verbose;
  public void log(string s) {
    if (verbose) {
      Console.WriteLine(s);
    }
  }

  public void initialize() {
    __default.InitState(out k, out hs);
    io = new DiskIOHandler();
  }

  public void crash() {
    log("'crashing' and reinitializing");
    log("");
    initialize();
  }

  public void Sync() {
    log("Sync");

    for (int i = 0; i < 50; i++) {
      __default.handleSync(k, hs, io, out bool success);
      if (success) {
        log("doing sync... success!");
        log("");
        return;
      } else {
        log("doing sync...");
      }
    }
    log("giving up");
    throw new Exception("operation didn't finish");
  }

  public void Insert(string key, string val) {
    log("Insert (\"" + key + "\", \"" + val + "\")");
    Insert(
      new Dafny.Sequence<byte>(string_to_bytes(key)),
      new Dafny.Sequence<byte>(string_to_bytes(val))
    );
  }

  public void Insert(byte[] key, byte[] val) {
    Insert(
      new Dafny.Sequence<byte>(key),
      new Dafny.Sequence<byte>(val)
    );
  }

  public void Insert(Dafny.Sequence<byte> key, Dafny.Sequence<byte> val) {
    for (int i = 0; i < 50; i++) {
      __default.handleInsert(k, hs, io, key, val, out bool success);
      if (success) {
        log("doing insert... success!");
        log("");
        return;
      } else {
        log("doing insert...");
      }
    }
    log("giving up");
    throw new Exception("operation didn't finish");
  }

  public void Query(string key) {
    byte[] val_bytes = Query(new Dafny.Sequence<byte>(string_to_bytes(key)));
    string val = bytes_to_string(val_bytes);
    log("Query result is: \"" + val + "\"");
  }

  public void Query(byte[] key) {
    Query(new Dafny.Sequence<byte>(key));
  }

  public void QueryAndExpect(byte[] key, byte[] expected) {
    byte[] actual = Query(new Dafny.Sequence<byte>(key));

    if (!byteArraysEqual(actual, expected)) {
      throw new Exception("did not get expected result\n");
    }
  }

  public byte[] Query(Dafny.Sequence<byte> key) {
    log("Query \"" + key + "\"");

    for (int i = 0; i < 50; i++) {
      __default.handleQuery(k, hs, io, key, out var result);
      if (result.is_Some) {
        byte[] val_bytes = result.dtor_value.Elements;
        log("doing query... success!");
        log("");
        return val_bytes;
      } else {
        log("doing query...");
      }
    }
    log("giving up");
    throw new Exception("operation didn't finish");
  }

  public static byte[] string_to_bytes(string s) {
    return System.Text.Encoding.UTF8.GetBytes(s);
  }

  public static string bytes_to_string(byte[] bytes) {
    return System.Text.Encoding.UTF8.GetString(bytes);
  }

  bool byteArraysEqual(byte[] a, byte[] b) {
    if (a.Length != b.Length) return false;
    for (int i = 0; i < a.Length; i++) {
      if (a[i] != b[i]) {
        return false;
      }
    }
    return true;
  }

}

public class FSUtil {
  public static void ClearIfExists() {
    if (System.IO.Directory.Exists(".veribetrfs-storage")) {
      System.IO.Directory.Delete(".veribetrfs-storage", true /* recursive */);
    } 
  }

  public static void Mkfs() {
    Dafny.Map<ulong, byte[]> m;
    MkfsImpl_Compile.__default.InitDiskBytes(out m);

    if (m.Count == 0) {
      throw new Exception("InitDiskBytes failed.");
    }

    if (System.IO.Directory.Exists(".veribetrfs-storage")) {
      throw new Exception("error: .veribetrfs-storage/ already exists");
    }
    System.IO.Directory.CreateDirectory(".veribetrfs-storage");

    DiskIOHandler io = new DiskIOHandler();

    foreach (ulong lba in m.Keys.Elements) {
      byte[] bytes = m.Select(lba);
      io.write(lba, bytes);
    }
  }
}

class Framework {
  public static void Run() {
    Application app = new Application();
    app.Insert("abc", "def");
    app.Insert("xyq", "rawr");
    app.Query("abc");
    app.Query("xyq");
    app.Query("blahblah");
    app.crash();
    app.Query("abc");
    app.Query("xyq");

    app.Insert("abc", "def");
    app.Insert("xyq", "rawr");
    app.Sync();
    app.crash();
    app.Query("abc");
    app.Query("xyq");

    for (int i = 0; i < 520; i++) {
      app.Insert("num" + i.ToString(), "llama");
    }

    app.Sync();
  }

  public static void Main(string[] args) {
    bool mkfs = false;
    bool benchmark = false;
    foreach (string arg in args) {
      if (arg.Equals("--mkfs")) {
        mkfs = true;
      }
      if (arg.Equals("--benchmark")) {
        benchmark = true;
      }
    }

    if (benchmark) {
      Benchmarks b = new Benchmarks();
      b.RunAllBenchmarks();
    } else if (mkfs) {
      FSUtil.Mkfs();
    } else {
      Run();
    }
  }
}

namespace Native_Compile {
  public partial class @Arrays
  {
      public static void @CopySeqIntoArray<A>(Dafny.Sequence<A> src, ulong srcIndex, A[] dst, ulong dstIndex, ulong len) {
          System.Array.Copy(src.Elements, (long)srcIndex, dst, (long)dstIndex, (long)len);
      }
  }
}
