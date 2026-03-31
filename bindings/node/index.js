/**
 * fastpdf2png — Ultra-fast PDF to PNG converter.
 *
 * Usage:
 *   const pdf = require("fastpdf2png");
 *
 *   const count = pdf.pageCount("doc.pdf");
 *   pdf.toFiles("doc.pdf", "output/", { dpi: 150 });
 *   const buffers = pdf.toBuffers("doc.pdf", { dpi: 150 });
 */

"use strict";

const { execFileSync, spawn } = require("child_process");
const path = require("path");
const fs = require("fs");
const os = require("os");

// Locate the binary: bundled platform package -> local build -> PATH
function findBinary() {
  const platform = os.platform();
  const arch = os.arch();
  const ext = platform === "win32" ? ".exe" : "";
  const platformPkg = `@fastpdf2png/${platform}-${arch}`;

  // 1. Platform-specific npm package (installed via optionalDependencies)
  try {
    return require.resolve(`${platformPkg}/fastpdf2png${ext}`);
  } catch (_) {}

  // 2. Local build (development)
  const localBin = path.join(__dirname, "..", "..", "build", `fastpdf2png${ext}`);
  if (fs.existsSync(localBin)) return localBin;

  // 3. Bundled in this package
  const bundledBin = path.join(__dirname, "bin", `fastpdf2png${ext}`);
  if (fs.existsSync(bundledBin)) return bundledBin;

  throw new Error(
    "fastpdf2png binary not found. Run: bash scripts/build.sh\n" +
    "Or install the platform package: npm install @fastpdf2png/" + platform + "-" + arch
  );
}

const BINARY = findBinary();
const MAX_WORKERS = Math.min(4, os.cpus().length || 1);

/**
 * Get the number of pages in a PDF (instant, no rendering).
 * @param {string} pdfPath
 * @returns {number}
 */
function pageCount(pdfPath) {
  const out = execFileSync(BINARY, ["--info", path.resolve(pdfPath)], {
    encoding: "utf-8",
  });
  return parseInt(out.trim(), 10);
}

/**
 * Convert a PDF to PNG files on disk.
 * @param {string} pdfPath
 * @param {string} outputDir
 * @param {{ dpi?: number, workers?: number, compression?: number, prefix?: string }} [options]
 * @returns {string[]} Array of output file paths
 */
function toFiles(pdfPath, outputDir, options = {}) {
  const dpi = options.dpi || 150;
  const workers = options.workers || MAX_WORKERS;
  const compression = options.compression ?? 2;
  const prefix = options.prefix || "page_";

  fs.mkdirSync(outputDir, { recursive: true });
  const pattern = path.join(path.resolve(outputDir), `${prefix}%03d.png`);

  execFileSync(BINARY, [
    path.resolve(pdfPath), pattern,
    String(dpi), String(workers),
    "-c", String(compression),
  ]);

  return fs.readdirSync(outputDir)
    .filter((f) => f.startsWith(prefix) && f.endsWith(".png"))
    .sort()
    .map((f) => path.join(outputDir, f));
}

/**
 * Convert a PDF to PNG buffers in memory.
 * @param {string} pdfPath
 * @param {{ dpi?: number, workers?: number, compression?: number }} [options]
 * @returns {Buffer[]} Array of PNG data buffers (one per page)
 */
function toBuffers(pdfPath, options = {}) {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "fp2p-"));
  try {
    const files = toFiles(pdfPath, tmpDir, options);
    return files.map((f) => fs.readFileSync(f));
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

/**
 * Persistent engine for batch processing.
 * Keeps PDFium loaded between calls.
 *
 * Usage:
 *   const engine = new pdf.Engine();
 *   const count = engine.pageCount("a.pdf");
 *   engine.toFiles("a.pdf", "out/");
 *   engine.close();
 */
class Engine {
  constructor() {
    this._proc = spawn(BINARY, ["--daemon"], {
      stdio: ["pipe", "pipe", "pipe"],
    });
    this._stdout = "";
    this._queue = [];  // FIFO queue of { resolve, reject } pairs
    this._proc.stdout.on("data", (data) => {
      this._stdout += data.toString();
      this._drain();
    });
  }

  /** Process buffered stdout lines against the pending queue. */
  _drain() {
    while (this._stdout.includes("\n") && this._queue.length > 0) {
      const nlIdx = this._stdout.indexOf("\n");
      const line = this._stdout.slice(0, nlIdx);
      this._stdout = this._stdout.slice(nlIdx + 1);
      const { resolve, reject } = this._queue.shift();
      if (line.startsWith("ERROR")) {
        reject(new Error(line));
      } else {
        resolve(line);
      }
    }
  }

  _cmd(command) {
    return new Promise((resolve, reject) => {
      if (!this._proc || this._proc.exitCode !== null) {
        return reject(new Error("Engine is closed"));
      }
      this._queue.push({ resolve, reject });
      this._proc.stdin.write(command + "\n");
    });
  }

  async pageCount(pdfPath) {
    const resp = await this._cmd(`INFO\t${path.resolve(pdfPath)}`);
    return parseInt(resp.split(" ")[1], 10);
  }

  async toFiles(pdfPath, outputDir, options = {}) {
    const dpi = options.dpi || 150;
    const workers = options.workers || MAX_WORKERS;
    const compression = options.compression ?? 2;
    const prefix = options.prefix || "page_";

    fs.mkdirSync(outputDir, { recursive: true });
    const pattern = path.join(path.resolve(outputDir), `${prefix}%03d.png`);

    await this._cmd(
      `RENDER\t${path.resolve(pdfPath)}\t${pattern}\t${dpi}\t${workers}\t${compression}`
    );

    return fs.readdirSync(outputDir)
      .filter((f) => f.startsWith(prefix) && f.endsWith(".png"))
      .sort()
      .map((f) => path.join(outputDir, f));
  }

  async toBuffers(pdfPath, options = {}) {
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "fp2p-"));
    try {
      const files = await this.toFiles(pdfPath, tmpDir, options);
      return files.map((f) => fs.readFileSync(f));
    } finally {
      fs.rmSync(tmpDir, { recursive: true, force: true });
    }
  }

  close() {
    if (this._proc && this._proc.exitCode === null) {
      this._proc.stdin.write("QUIT\n");
      this._proc.stdin.end();
    }
    this._proc = null;
    // Reject any pending commands
    for (const { reject } of this._queue) {
      reject(new Error("Engine closed"));
    }
    this._queue = [];
  }
}

module.exports = { pageCount, toFiles, toBuffers, Engine };
