import { runtime } from 'rime:runtime'; if (runtime.ping() !== 'pong') throw new Error('no pong');
