const x  = [0.0, 4.84143144246472090, 8.34336671824457987, 12.8943695621391310, 15.3796971148509165];
const y  = [0.0, -1.16032004402742839, 4.12479856412430479, -15.1111514016986312, -25.9193146099879641];
const z  = [0.0, -0.103622044471123109, -0.403523417114321381, -0.223307578892655734, 0.179258772950371181];
const vx = [0.0, 0.606326392995832, -1.0107743461787924, 1.0827910064415354, 0.979090732243898];
const vy = [0.0, 2.81198684491626, 1.8256623712304119, 0.8687130181696082, 0.5946989986476762];
const vz = [0.0, -0.02521836165988763, 0.008415761376584154, -0.010832637401363636, -0.034755955504078104];
const m  = [39.47841760435743, 0.03769367487038949, 0.011286326131968767, 0.0017237240570597112, 0.0020336868699246304];
function energy() {
  let e = 0.0, i = 0;
  while (i < 5) {
    e = e + 0.5 * m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
    let j = i + 1;
    while (j < 5) {
      const dx = x[i] - x[j], dy = y[i] - y[j], dz = z[i] - z[j];
      e = e - m[i] * m[j] / Math.sqrt(dx * dx + dy * dy + dz * dz);
      j = j + 1;
    }
    i = i + 1;
  }
  return e;
}
function advance(dt) {
  let i = 0;
  while (i < 5) {
    let j = i + 1;
    while (j < 5) {
      const dx = x[i] - x[j], dy = y[i] - y[j], dz = z[i] - z[j];
      const d2 = dx * dx + dy * dy + dz * dz;
      const mag = dt / (d2 * Math.sqrt(d2));
      vx[i] = vx[i] - dx * m[j] * mag;
      vy[i] = vy[i] - dy * m[j] * mag;
      vz[i] = vz[i] - dz * m[j] * mag;
      vx[j] = vx[j] + dx * m[i] * mag;
      vy[j] = vy[j] + dy * m[i] * mag;
      vz[j] = vz[j] + dz * m[i] * mag;
      j = j + 1;
    }
    i = i + 1;
  }
  i = 0;
  while (i < 5) {
    x[i] = x[i] + dt * vx[i];
    y[i] = y[i] + dt * vy[i];
    z[i] = z[i] + dt * vz[i];
    i = i + 1;
  }
}
console.log(String(Math.floor(energy() * 1000000000)));
let s = 0;
while (s < 40000) { advance(0.01); s = s + 1; }
console.log(String(Math.floor(energy() * 1000000000)));
