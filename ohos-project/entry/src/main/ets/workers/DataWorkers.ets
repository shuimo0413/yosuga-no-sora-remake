/**
 * TaskPool workers for the data bootstrap flow (download / import).
 *
 * Extracted from pages/Index.ets: the page used to carry ~320 lines of
 * background-worker code that had nothing to do with the UI. Every function
 * here is a module-level @Concurrent worker (the TaskPool requirement) that
 * only references imported symbols and its own locals, so the set is safe
 * to call through taskpool.execute from any page.
 */

import { fileIo as fs } from '@kit.CoreFileKit';
import zlib from '@ohos.zlib';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';

/**
 * TaskPool worker: decompress a zip on a background thread. The UI thread
 * must not run zlib.decompressFile on a multi-GB archive: the long
 * synchronous call triggers ANR and the app gets killed mid-import.
 */
@Concurrent
export async function unzipTask(srcZip: string, outDir: string): Promise<string> {
  try {
    // decompressFile is ASYNC (Promise-only API). Without this await the
    // TaskPool worker returned immediately and the decompression was
    // cancelled after the first couple of entries ("ok:" with an empty
    // list, and a truncated data/ tree).
    await zlib.decompressFile(srcZip, outDir);
    // Report the top-level entries so a truncated decompression is
    // visible in the debug log instead of failing silently.
    let names: string[] = [];
    try {
      names = fs.listFileSync(outDir);
    } catch (e) {
      names = ['<listerr:' + JSON.stringify(e) + '>'];
    }
    return 'ok:' + names.join(',');
  } catch (e) {
    return 'err:' + JSON.stringify(e);
  }
}

/** TaskPool worker: copy one file (multi-GB) off the UI thread. */
@Concurrent
export function copyFileTask(src: string, dst: string): void {
  try {
    fs.copyFileSync(src, dst);
  } catch (e) {
    // ignore
  }
}

/** TaskPool worker: sha256 of a multi-GB file. Reading a 1.7GB zip
 *  chunk-by-chunk on the UI thread blocks it long enough to trip
 *  THREAD_BLOCK_6S (the system kills the app right after a download
 *  finishes). Returns the hex digest or 'err:<json>'. */
@Concurrent
export async function sha256Task(path: string): Promise<string> {
  try {
    const md = cryptoFramework.createMd('SHA256');
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY);
    const chunk = new ArrayBuffer(1024 * 1024);
    try {
      while (true) {
        const len = fs.readSync(file.fd, chunk);
        if (len <= 0) {
          break;
        }
        const view = new Uint8Array(chunk.slice(0, len));
        await md.update({ data: view });
      }
    } finally {
      fs.closeSync(file);
    }
    const digest = await md.digest();
    const bytes = digest.data;
    let hex = '';
    for (let i = 0; i < bytes.length; i++) {
      hex += bytes[i].toString(16).padStart(2, '0');
    }
    return hex.toUpperCase();
  } catch (e) {
    return 'err:' + JSON.stringify(e);
  }
}

/** TaskPool worker: copy a picked uri (content://) into the sandbox
 *  cache. The synchronous uri read can block on datashare IPC for
 *  seconds; running it on the UI thread trips THREAD_BLOCK_6S and the
 *  system kills the app. Returns 'ok' or 'err:<json>'. */
@Concurrent
export async function copyUriTask(uri: string, dest: string): Promise<string> {
  try {
    // Prefer copyFileSync: it resolves the picker grant through the
    // datashare/file-manager path. fs.openSync(uri) on KaihongOS goes
    // through the MediaLibrary permission check instead and is denied
    // (-13 / 13900002) for Download files, hanging the import.
    try {
      fs.copyFileSync(uri, dest);
      return 'ok';
    } catch (e) {
      // fall through to the streaming copy below
    }
    let src: fs.File | null = null;
    try {
      src = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    } catch (e) {
      return 'err:' + JSON.stringify(e);
    }
    const dst = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    const buf = new ArrayBuffer(1024 * 1024);
    try {
      while (true) {
        const n = fs.readSync(src.fd, buf);
        if (n <= 0) {
          break;
        }
        fs.writeSync(dst.fd, buf.slice(0, n));
      }
    } finally {
      fs.closeSync(src);
      fs.closeSync(dst);
    }
    return 'ok';
  } catch (e) {
    return 'err:' + JSON.stringify(e);
  }
}

/**
 * TaskPool worker: move a directory tree across volumes (copy each file,
 * then remove the source tree). Used when the sandbox staging dir must
 * become the public data/ folder (a rename fails with EXDEV across
 * volumes). Runs off the UI thread.
 */
@Concurrent
export function moveTreeTask(srcDir: string, dstDir: string): void {
  const progressPath = srcDir + '.move.progress';
  const writeProgress = (done: number, total: number): void => {
    try {
      const fd = fs.openSync(progressPath,
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(fd.fd, done + ' ' + total);
      fs.closeSync(fd);
    } catch (e) {
      // ignore
    }
  };

  // Pass 1: count files for the progress denominator.
  let total = 0;
  const cStack: string[] = [''];
  while (cStack.length > 0) {
    const rel = cStack.pop();
    if (rel === undefined) {
      continue;
    }
    const src = srcDir + (rel === '' ? '' : '/' + rel);
    let entries: string[] = [];
    try {
      entries = fs.listFileSync(src);
    } catch (e) {
      continue;
    }
    for (const name of entries) {
      const s = src + '/' + name;
      try {
        if (fs.statSync(s).isDirectory()) {
          cStack.push(rel === '' ? name : rel + '/' + name);
        } else {
          total++;
        }
      } catch (e) {
        // ignore
      }
    }
  }

  // Pass 2: copy files (with progress), then remove the source tree.
  let done = 0;
  writeProgress(0, total);
  const stack: string[] = [''];
  while (stack.length > 0) {
    const rel = stack.pop();
    if (rel === undefined) {
      continue;
    }
    const src = srcDir + (rel === '' ? '' : '/' + rel);
    const dst = dstDir + (rel === '' ? '' : '/' + rel);
    let entries: string[] = [];
    try {
      entries = fs.listFileSync(src);
    } catch (e) {
      continue;
    }
    try {
      fs.mkdirSync(dst, true);
    } catch (e) {
      // may already exist
    }
    for (const name of entries) {
      const s = src + '/' + name;
      const d = dst + '/' + name;
      let st: fs.Stat | null = null;
      try {
        st = fs.statSync(s);
      } catch (e) {
        continue;
      }
      if (st.isDirectory()) {
        stack.push(rel === '' ? name : rel + '/' + name);
      } else {
        try {
          fs.copyFileSync(s, d);
          fs.unlinkSync(s);
        } catch (e) {
          // a retry on the next launch picks it up
          try {
            const ef = fs.openSync(srcDir + '.move.err',
              fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.APPEND);
            fs.writeSync(ef.fd, d + ' | ' + JSON.stringify(e) + '\n');
            fs.closeSync(ef);
          } catch (e2) {
            // ignore
          }
        }
        done++;
        if (done % 512 === 0 || done === total) {
          writeProgress(done, total);
        }
      }
    }
  }
  // Remove the (now empty) source tree bottom-up.
  const dirs: string[] = [];
  const rmStack: string[] = [srcDir];
  while (rmStack.length > 0) {
    const cur = rmStack.pop();
    if (cur === undefined || cur === '') {
      continue;
    }
    let entries: string[] = [];
    try {
      entries = fs.listFileSync(cur);
    } catch (e) {
      continue;
    }
    for (const name of entries) {
      const child = cur + '/' + name;
      try {
        fs.unlinkSync(child);
      } catch (e) {
        dirs.push(child);
        rmStack.push(child);
      }
    }
  }
  dirs.sort((a: string, b: string): number => b.length - a.length);
  for (const d of dirs) {
    try {
      fs.rmdirSync(d);
    } catch (e) {
      // ignore
    }
  }
  try {
    fs.rmdirSync(srcDir);
  } catch (e) {
    // ignore
  }

  // Completion marker.
  try {
    const fd = fs.openSync(progressPath,
      fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    fs.writeSync(fd.fd, 'DONE');
    fs.closeSync(fd);
  } catch (e) {
    // ignore
  }
}

/**
 * TaskPool worker: delete a directory tree iteratively (no recursion:
 * @Concurrent functions may only reference imported or local variables).
 * Runs off the UI thread because leftover krkr_* video temp dirs can be
 * hundreds of MB.
 */
@Concurrent
export function removeTreeTask(path: string): void {
  const dirs: string[] = [];
  const stack: string[] = [path];
  while (stack.length > 0) {
    const cur = stack.pop();
    if (cur === undefined || cur === '') {
      continue;
    }
    let entries: string[] = [];
    try {
      entries = fs.listFileSync(cur);
    } catch (e) {
      continue;
    }
    for (const name of entries) {
      const child = cur + '/' + name;
      try {
        fs.unlinkSync(child);
      } catch (e) {
        // not a plain file (directory) - descend into it later
        dirs.push(child);
        stack.push(child);
      }
    }
  }
  // Remove deepest directories first: a child path is always longer than
  // its parent, so sorting by path length descending visits children
  // before parents and rmdir (empty-only) succeeds bottom-up.
  dirs.sort((a: string, b: string): number => b.length - a.length);
  for (const d of dirs) {
    try {
      fs.rmdirSync(d);
    } catch (e) {
      // ignore
    }
  }
  try {
    fs.rmdirSync(path);
  } catch (e) {
    // ignore
  }
}
