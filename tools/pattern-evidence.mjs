// Judge native geometry, documents and CSP1 codes against the read-only browser implementation.
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {createRequire} from 'node:module';
const require=createRequire(import.meta.url);
const ref=path.resolve(import.meta.dirname,'../../CSSTeroids-2');
const {findRegions,regionAt,repeatPattern,mesh,patternLines,createPatternTile,decodePatternCode,encodePatternCode}=await import(pathToFileURL(path.join(ref,'public/js/ship-customizer/editor-lab/geometry.mjs')));
const {validateEditorDocument}=require(path.join(ref,'utils/ship-editor-document.js'));
const fixtures=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
let segments=0;
function compare(actual,expected,label){
  if(actual.length!==expected.length)throw Error(`${label}: ${actual.length} native segments vs ${expected.length} browser segments`);
  const wanted=expected.slice(),near=(a,b)=>Math.abs(a-b)<1e-4;
  for(const line of actual){
    const same=(a,b)=>near(a.startX,b.startX)&&near(a.startY,b.startY)&&near(a.endX,b.endX)&&near(a.endY,b.endY);
    const at=wanted.findIndex(b=>same(line,b)||same(line,{startX:b.endX,startY:b.endY,endX:b.startX,endY:b.startY}));
    if(at<0)throw Error(`${label}: native segment absent from browser output ${JSON.stringify(line)}`);
    wanted.splice(at,1);segments++;
  }
}
for(const [i,f] of fixtures.entries()){
  const region=regionAt(findRegions(f.hull),f.seed);if(!region)throw Error('Browser found no closed region');
  compare(f.repeated,repeatPattern(region,f.tile,f.scale),`repeat ${i} ${f.preset}`);
  compare(f.mesh,mesh(region,f.preset,20),`mesh ${i} ${f.preset}`);
  compare(f.tile.lines,patternLines(createPatternTile({preset:f.preset,presetSpacing:2,color:f.tile.color,strokeWidth:f.tile.strokeWidth})),`starter ${f.preset}`);
  const validation=validateEditorDocument(f.document);if(!validation.valid)throw Error(validation.message);
  if(!f.code)continue; // dense tile codes can exceed the shared 4096 character cap
  const decoded=decodePatternCode(f.code);compare(decoded.lines,f.tile.lines,`CSP1 ${i}`);
  // The browser's canonical encoding is exercised too; the C++ decoder round-trip is host tested.
  if(decodePatternCode(encodePatternCode(decoded)).lines.length!==decoded.lines.length)throw Error('Browser code round-trip failed');
}
console.log(`Pattern browser parity passed: ${fixtures.length} documents/regions, ${segments} matched segments; server documents and CSP1 codes accepted`);
const browserCodes=[];
for(const preset of ['grid','hex','triangle','diamond','ribs','crosshatch']){
  const tile=createPatternTile({preset,presetSpacing:2,color:'#00ffff',strokeWidth:1});
  browserCodes.push({code:encodePatternCode(tile),lines:patternLines(tile)});
}
const freeform=createPatternTile({name:'Browser tile',edges:['0,0|1,0'],lines:[{id:'custom-1',startX:1,startY:2,endX:7,endY:6,color:'#ff00ff',width:2}]});
browserCodes.push({code:encodePatternCode(freeform),lines:patternLines(freeform)});
fs.writeFileSync(process.argv[2]+'.browser-codes.json',JSON.stringify(browserCodes));
