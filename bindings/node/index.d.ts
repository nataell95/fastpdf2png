export interface Options {
  dpi?: number;
  workers?: number;
  compression?: number;
  prefix?: string;
}

/** Get page count (instant, no rendering). */
export function pageCount(pdfPath: string): number;

/** Convert PDF to PNG files on disk. Returns array of file paths. */
export function toFiles(pdfPath: string, outputDir: string, options?: Options): string[];

/** Convert PDF to PNG buffers in memory. Returns array of Buffer. */
export function toBuffers(pdfPath: string, options?: Options): Buffer[];

/** Persistent engine for batch processing. */
export class Engine {
  constructor();
  pageCount(pdfPath: string): Promise<number>;
  toFiles(pdfPath: string, outputDir: string, options?: Options): Promise<string[]>;
  toBuffers(pdfPath: string, options?: Options): Promise<Buffer[]>;
  close(): void;
}
