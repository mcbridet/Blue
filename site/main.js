(() => {
  // Windowshade: the collapse box, or a double-click on the title bar, rolls a window up
  for (const win of document.querySelectorAll('.window')) {
    const button = win.querySelector('.shade');
    const content = win.querySelector('.window-content');
    if (!button || !content) continue;
    const toggle = () => {
      const open = button.getAttribute('aria-expanded') === 'true';
      button.setAttribute('aria-expanded', String(!open));
      button.setAttribute('aria-label', open ? 'Expand window' : 'Collapse window');
      content.hidden = open;
      win.classList.toggle('collapsed', open);
    };
    button.addEventListener('click', toggle);
    win.querySelector('.titlebar').addEventListener('dblclick', (e) => {
      if (e.target !== button) toggle();
    });
  }

  // Copy buttons on code blocks
  if (navigator.clipboard) {
    for (const pre of document.querySelectorAll('pre.code')) {
      const wrap = document.createElement('div');
      wrap.className = 'codeblock';
      pre.replaceWith(wrap);
      wrap.append(pre);

      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'button small copy';
      button.textContent = 'Copy';
      button.addEventListener('click', async () => {
        try {
          await navigator.clipboard.writeText(pre.innerText.trimEnd());
          button.textContent = 'Copied';
        } catch {
          button.textContent = 'Failed';
        }
        setTimeout(() => { button.textContent = 'Copy'; }, 1500);
      });
      wrap.append(button);
    }
  }
})();
