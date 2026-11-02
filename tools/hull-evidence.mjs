// Compare the PS5 study starters with the browser's actual sample() implementation.
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {createRequire} from 'node:module';
const ref=path.resolve(import.meta.dirname,'../../CSSTeroids-2');
const {segment}=await import(pathToFileURL(path.join(ref,'public/js/ship-customizer/editor-lab/geometry.mjs')));
const {validateShipData}=createRequire(import.meta.url)(path.join(ref,'utils/helpers.js'));
const source=fs.readFileSync(path.join(ref,'public/js/ship-customizer/editor-lab/editor.mjs'),'utf8');
const start=source.indexOf('function sample(name){'),end=source.indexOf('function load(',start);
if(start<0||end<0)throw Error('Browser sample() location changed; inspect the reference');
let id=0;
const sample=new Function('segment','uid',source.slice(start,end)+';return sample;')(segment,()=>`reference-${id++}`);
const hulls=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
for(const hull of hulls){
  if(hull.preset==='BLANK'){if(hull.customLines.length)throw Error('Blank starter has linework');continue;}
  const accepted=validateShipData(hull);if(!accepted.valid)throw Error(accepted.message);
  if(!['MERIDIAN','MANTA','DELTA'].includes(hull.preset))continue;
  const browser=sample(hull.preset.toLowerCase());
  const extent=Math.max(...browser.customLines.flatMap(l=>[l.startX,l.startY,l.endX,l.endY].map(Math.abs)));
  const scale=extent>200?180/extent:1;
  if(hull.customLines.length!==browser.customLines.length)throw Error('Starter line count differs');
  for(let i=0;i<hull.customLines.length;i++){
    const a=hull.customLines[i],b=browser.customLines[i];
    for(const key of ['startX','startY','endX','endY'])if(Math.abs(a[key]-b[key]*scale)>1/1024)throw Error(`${hull.preset} ${key} differs`);
    if(a.color!==b.color||a.width!==b.width)throw Error('Starter style differs');
  }
}
console.log('Starter hull checks passed: seven choices; Meridian/Manta/Delta match browser shapes fitted to the PS5 drawing square');
