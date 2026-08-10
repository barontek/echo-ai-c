import js from '@eslint/js';
import globals from 'globals';
import jsdoc from 'eslint-plugin-jsdoc';
import reactHooks from 'eslint-plugin-react-hooks';
import reactRefresh from 'eslint-plugin-react-refresh';
import tseslint from 'typescript-eslint';
import { defineConfig, globalIgnores } from 'eslint/config';

export default defineConfig([
  globalIgnores(['dist']),
  {
    files: ['**/*.{ts,tsx}'],
    extends: [
      js.configs.recommended,
      tseslint.configs.recommended,
      reactHooks.configs.flat.recommended,
      reactRefresh.configs.vite,
      jsdoc.configs['flat/recommended'],
    ],
    languageOptions: {
      ecmaVersion: 2020,
      globals: globals.browser,
    },
    // TSDoc is the documented contract for every export (AGENTS.md
    // "TypeScript/React — TSDoc style"): require-jsdoc enforces a doc
    // comment on exported symbols. Kept at 'warn' — flip to 'error' only
    // once every export carries a compliant comment. The param/returns
    // formatter rules are off: props and return types are already in the
    // signatures, AGENTS.md says not to restate them, and the rules
    // fight destructured props and getters.
    rules: {
      'jsdoc/require-jsdoc': [
        'warn',
        {
          publicOnly: true,
          require: {
            FunctionDeclaration: true,
            MethodDefinition: true,
            ClassDeclaration: true,
            ArrowFunctionExpression: true,
            ClassExpression: true,
            FunctionExpression: true,
          },
        },
      ],
      'jsdoc/multiline-blocks': 'off',
      'jsdoc/require-param': 'off',
      'jsdoc/require-returns': 'off',
      'jsdoc/require-returns-check': 'off',
      'jsdoc/check-param-names': 'off',
      'jsdoc/no-undefined-types': 'off',
      'jsdoc/require-param-type': 'off',
      'jsdoc/require-returns-type': 'off',
      'jsdoc/tag-lines': ['warn', 'any', { startLines: 1 }],
    },
  },
]);
