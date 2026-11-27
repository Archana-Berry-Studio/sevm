import * as os from 'mbjs:os';
console.log('memoryInfo =>', JSON.stringify(os.memoryInfo()));
console.log('now =>', os.now());
console.log('platform =>', os.platform);
console.log('exePath =>', String(os.exePath()));
