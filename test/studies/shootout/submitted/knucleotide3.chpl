/* The Computer Language Benchmarks Game
   https://salsa.debian.org/benchmarksgame-team/benchmarksgame/

   contributed by Ben Harshbarger and Brad Chamberlain
   derived from the GNU C++ version by Branimir Maksimovic
*/

use IO, Map, Sort;

config param tableSize = 2**16,
             columns = 61;


proc main(args: [] string) {
  // Create a non-locking version of 'stdin' and query its size
  const consoleIn = new file(0),
        fileLen = consoleIn.size,
        stdin = consoleIn.reader(locking=false);

  // Read line-by-line until we see a line beginning with '>TH'
  var buff: [1..columns] uint(8),
      lineSize = 0,
      numRead = 0;

  do {
    lineSize = stdin.readLine(buff);
    numRead += lineSize;
  } while lineSize > 0 && !startsWithThree(buff);

  // Read in the rest of the file
  var dataDom = {1..fileLen-numRead},
      data: [dataDom] uint(8),
      idx = 1;

  do {
    lineSize = stdin.readLine(data[idx..], stripNewline=true);
    idx += lineSize;
  } while lineSize > 0;

  // Resize our array to the amount actually read
  dataDom = {1..idx};

  // Make everything uppercase
  forall d in data do
    d -= ("a".toByte() - "A".toByte());

  writeFreqs(data, 1);
  writeFreqs(data, 2);
  writeCount(data, "GGT");
  writeCount(data, "GGTA");
  writeCount(data, "GGTATT");
  writeCount(data, "GGTATTTTAATT");
  writeCount(data, "GGTATTTTAATTTATAGT");
}


proc writeFreqs(data, param nclSize) {
  const freqs = calculate(data, nclSize);

  // create an array of (frequency, sequence) tuples
  var arr = for (s,f) in zip(freqs.keys(), freqs.values()) do (f,s);

  // print the array, sorted by decreasing frequency
  sort(arr, new reverseComparator());
  for (f, s) in arr do
   writef("%s %.3dr\n", decode(s, nclSize),
           (100.0 * f) / (data.size - nclSize));
  writeln();
}


proc writeCount(data, param str) {
  const strBytes = str.bytes(),
        freqs = calculate(data, str.numBytes),
        d = hash(strBytes, strBytes.domain.low, str.numBytes);

  writeln(freqs.get(d, 0), "\t", decode(d, str.numBytes));
}


proc calculate(data, param nclSize) {
  var freqs = new map(int, int);

  var lock: sync bool = true;
  const numTasks = here.maxTaskPar;
  coforall tid in 1..numTasks with (ref freqs) {
    var myFreqs = new map(int, int);

    for i in tid..(data.size - nclSize) by numTasks do
      myFreqs[hash(data, i, nclSize)] += 1;

    lock.readFE();      // acquire lock
    for (k,v) in zip(myFreqs.keys(), myFreqs.values()) do
      freqs[k] += v;
    lock.writeEF(true); // release lock
  }

  return freqs;
}


const toChar: [0..3] string = ["A", "C", "T", "G"];
var toNum: [0..127] int;

forall i in toChar.domain do
  toNum[toChar[i].toByte()] = i;


inline proc decode(in data, param nclSize) {
  var ret: string;

  for i in 1..nclSize {
    ret = toChar[(data & 3)] + ret;
    data >>= 2;
  }

  return ret;
}


inline proc hash(str, beg, param size) {
  var data = 0;

  for i in 0..size-1 {
    data <<= 2;
    data |= toNum[str[beg+i]];
  }

  return data;
}


inline proc startsWithThree(data) {
  return data[1] == ">".toByte() &&
         data[2] == "T".toByte() &&
         data[3] == "H".toByte();
}

