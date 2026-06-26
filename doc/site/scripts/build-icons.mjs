import { copyFile, mkdir } from 'node:fs/promises';
import { dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';

const source = new URL('../../logo.png', import.meta.url);
const publicDir = new URL('../public/', import.meta.url);

const icons = [
  ['favicon-16x16.png', 16],
  ['favicon-32x32.png', 32],
  ['apple-touch-icon.png', 180],
  ['icon-192.png', 192]
];

await mkdir(fileURLToPath(publicDir), { recursive: true });
await copyFile(source, new URL('logo.png', publicDir));

await Promise.all(
  icons.map(([filename, size]) =>
    sharp(fileURLToPath(source))
      .resize(size, size, { fit: 'cover' })
      .png()
      .toFile(fileURLToPath(new URL(filename, publicDir)))
  )
);

console.log(`Built ${icons.length + 1} logo assets in ${dirname(fileURLToPath(new URL('logo.png', publicDir)))}`);
