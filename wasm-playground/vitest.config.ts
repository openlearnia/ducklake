import { defineConfig } from 'vitest/config';
import viteConfig from './vite.config';

export default defineConfig({
  ...viteConfig,
  test: {
    exclude: ['node_modules', 'dist', 'tests/**'],
  },
});
