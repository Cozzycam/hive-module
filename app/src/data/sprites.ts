// Sprite grid definitions for pixel art rendering
// These map to the firmware's sprite frames defined in conker.h
// In v1 we use simple SVG representations; actual pixel sprites
// will be ported from the design/sprites/ JSON files in a future pass.

export const SPRITE_SIZE = 16; // pixels per sprite cell

export interface SpriteFrame {
  name: string;
  width: number;
  height: number;
}

export const SPRITES: Record<string, SpriteFrame> = {
  worker: { name: 'WORKER', width: 8, height: 8 },
  worker_lean: { name: 'WORKER_LEAN', width: 8, height: 8 },
  queen: { name: 'QUEEN', width: 16, height: 16 },
};
