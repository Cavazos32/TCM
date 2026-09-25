import {StrictMode} from 'react';
import {createRoot} from 'react-dom/client';
import App from './App.tsx';
import './index.css';

const rootEl = document.getElementById('root');
if (!rootEl) {
  throw new Error('No existe #root');
}

try {
  createRoot(rootEl).render(
    <StrictMode>
      <App />
    </StrictMode>,
  );
} catch (err) {
  rootEl.textContent = err instanceof Error ? err.stack || err.message : String(err);
}
