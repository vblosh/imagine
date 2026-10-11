import fs from 'fs';
import path from 'path';

const webDir = path.resolve('web');
const files = fs.readdirSync(webDir).filter(f => f.endsWith('.js'));

const importGraph = new Map();

for (const file of files) {
  const filePath = path.join(webDir, file);
  const content = fs.readFileSync(filePath, 'utf-8');
  const imports = [];
  const regex = /(?:import|export)\s+(?:.*?from\s+)?['"]\.\/([^'"]+)['"]/g;
  let match;
  while ((match = regex.exec(content)) !== null) {
    const imported = match[1];
    // ignore non-js or subdirs like vendor
    if (imported.endsWith('.js') && !imported.includes('/')) {
      imports.push(imported);
    }
  }
  importGraph.set(file, imports);
}

function findCycles() {
  const cycles = [];
  const visited = new Set();
  const recStack = [];

  function dfs(node) {
    visited.add(node);
    recStack.push(node);

    const neighbors = importGraph.get(node) || [];
    for (const neighbor of neighbors) {
      if (!visited.has(neighbor)) {
        dfs(neighbor);
      } else if (recStack.includes(neighbor)) {
        const cycleStartIndex = recStack.indexOf(neighbor);
        const cycle = recStack.slice(cycleStartIndex).concat(neighbor);
        cycles.push(cycle);
      }
    }

    recStack.pop();
  }

  for (const node of importGraph.keys()) {
    visited.clear();
    recStack.length = 0;
    dfs(node);
  }

  // Deduplicate cycles
  const uniqueCycles = [];
  const seen = new Set();
  for (const cycle of cycles) {
    // normalize cycle representation
    const cycleNodes = cycle.slice(0, -1);
    const minIndex = cycleNodes.indexOf([...cycleNodes].sort()[0]);
    const normalized = cycleNodes.slice(minIndex).concat(cycleNodes.slice(0, minIndex)).join(' -> ');
    if (!seen.has(normalized)) {
      seen.add(normalized);
      uniqueCycles.push(cycle);
    }
  }

  return uniqueCycles;
}

const cycles = findCycles();
console.log(`Found ${cycles.length} circular dependencies:`);
for (const cycle of cycles) {
  console.log('  ' + cycle.join(' -> '));
}

if (cycles.length > 0) {
  process.exit(1);
} else {
  console.log('No cycles found! All modules are cycle-free.');
  process.exit(0);
}
