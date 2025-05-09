use IO;

var f = openTempFile();

{
  var w = f.writer(locking=false);
  w.writeByte(0x11);
}

{
  var r = f.reader(locking=false);
  r.readByte(); // should not throw
  try {
    r.readByte(); // should throw, fileReader is at EOF
    writeln("Incorrectly did not throw");
  } catch e:EofError {
    writeln("Correctly threw EofError");
  } catch { 
    writeln("Incorrectly threw some other error");
  }
}
