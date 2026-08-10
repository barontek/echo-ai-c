import '@testing-library/jest-dom/vitest';
import { cleanup } from '@testing-library/react';
import { afterEach, beforeEach } from 'vitest';

/* jsdom does not provide a working localStorage in this environment;
 * ApiClient and ChatProvider read/write it, so install an in-memory
 * shim at import time (the api singleton is constructed at module
 * scope), and reset per test to keep cases independent. */
class MemoryStorage implements Storage {
  private store = new Map<string, string>();

  get length(): number {
    return this.store.size;
  }

  clear(): void {
    this.store.clear();
  }

  getItem(key: string): string | null {
    return this.store.get(key) ?? null;
  }

  key(index: number): string | null {
    return Array.from(this.store.keys())[index] ?? null;
  }

  removeItem(key: string): void {
    this.store.delete(key);
  }

  setItem(key: string, value: string): void {
    this.store.set(key, String(value));
  }
}

Object.defineProperty(window, 'localStorage', {
  configurable: true,
  value: new MemoryStorage(),
});

beforeEach(() => {
  window.localStorage.clear();
});

afterEach(() => {
  cleanup();
});
