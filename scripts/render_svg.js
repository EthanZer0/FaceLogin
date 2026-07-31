const sharp = require('sharp');
const fs = require('fs');

// Read SVG, set all paths to white fill
let svg = fs.readFileSync('assets/face.svg', 'utf-8');
svg = svg.replace(/<path /g, '<path fill="#ffffff" ');

// Render SVG to 128x128 RGBA raw pixels
sharp(Buffer.from(svg), {density: 300})
    .resize(128, 128)
    .raw()
    .toBuffer()
    .then(data => {
        // data is RGBA, convert to BGRA for BMP
        const bgra = Buffer.alloc(data.length);
        for (let i = 0; i < data.length; i += 4) {
            bgra[i]     = data[i+2]; // B
            bgra[i+1]   = data[i+1]; // G
            bgra[i+2]   = data[i];   // R
            bgra[i+3]   = data[i+3]; // A
        }
        fs.writeFileSync('assets/tile_data.bin', bgra);
        const nz = [...bgra].filter((_, i) => i % 4 === 3 && _ > 0).length;
        console.log(`tile_data.bin: ${bgra.length} bytes, alpha>0: ${nz}/16384`);
    })
    .catch(e => { console.error(e.message); process.exit(1); });
