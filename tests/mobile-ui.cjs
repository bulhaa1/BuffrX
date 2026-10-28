// Run with Playwright installed and the site served at http://127.0.0.1:4173.
// Override the server address with SITE_URL when needed.
const assert = require('node:assert/strict');
const { test } = require('node:test');
const { chromium } = require('playwright');

const url = process.env.SITE_URL || 'http://127.0.0.1:4173';
const cases = [
  ['small phone', 320, 740, true, true],
  ['phone', 390, 844, true, true],
  ['landscape phone', 844, 390, true, true],
  ['portrait tablet', 768, 1024, true, false],
  ['landscape tablet', 1024, 768, true, false],
  ['desktop', 1440, 1000, false, false],
];

for (const [name, width, height, hasTouch, phone] of cases) {
  test(`${name}: copy, motion, controls and overflow`, async () => {
    const browser = await chromium.launch({ executablePath: chromium.executablePath() });
    try {
      const page = await browser.newPage({ viewport: { width, height }, hasTouch });
      const errors = [];
      page.on('pageerror', (e) => errors.push(e.message));
      await page.goto(url);
      await page.waitForFunction(() => document.querySelectorAll('.fcard').length >= 3);
      const intro = page.locator('#features .sec-head .lede');
      assert.equal(await intro.isVisible(), !phone, 'Phone-only filler must not affect tablet or desktop');
      assert.equal(await page.locator('.claim p').first().isVisible(), !phone);
      assert.equal(await page.locator('#pause').isVisible(), true, 'Autoplay always needs a pause control');
      assert.equal(await page.locator('#why .fine').isVisible(), true, 'Release and platform details stay visible');
      assert.equal(await page.locator('.reqs-box').isVisible(), !phone, 'Hide bottom requirements only on phones');
      assert.equal(await page.locator('.site-foot').isVisible(), !phone, 'Hide footer strip only on phones');
      assert.equal(await page.locator('#qa .q:visible').count(), phone ? 4 : 11);
      if (phone) {
        assert.deepEqual(await page.locator('#qa .q:visible h3 button > span:first-child').allTextContents(), [
          'Is it really free? What is the catch?',
          'Is it Windows only? What about Linux and macOS?',
          'Does it collect my data?',
          'How does Insta Share work? Does my friend need BuffrX?',
        ]);
        assert.ok((await page.locator('#themebtn').boundingBox()).width <= 56, 'Theme trigger should be compact');
        await page.locator('#themebtn').click();
        const picker = await page.locator('#pop').boundingBox();
        assert.ok(picker.width <= 288 && picker.x >= 0 && picker.x + picker.width <= width);
        await page.locator('#siteThemes [data-id="sandstorm"]').click();
        await page.waitForFunction(() => document.documentElement.dataset.theme === 'sandstorm');
        await page.keyboard.press('Escape');
        assert.equal(await page.locator('#pop').isVisible(), false);
        assert.equal(await page.locator('.fcopy .head').first().isVisible(), false);
        assert.match(await page.locator('.fcard.star .facts:visible').innerText(), /4 GB/);
        assert.match(await page.locator('.fcard.star .facts:visible').innerText(), /PC is on/);
        const heading = page.locator('#features .sec-head');
        assert.equal(await heading.evaluate((e) => e.classList.contains('in')), false, 'Offscreen sections must wait for scrolling');
        await heading.scrollIntoViewIfNeeded();
        await page.waitForFunction(() => document.querySelector('#features .sec-head').classList.contains('in'));
        const motion = await heading.locator('h2').evaluate((e) => {
          const s = getComputedStyle(e);
          return { duration: s.transitionDuration, filter: s.filter };
        });
        assert.notEqual(motion.duration, '0s', 'Phone headings should animate on entry');
        assert.equal(motion.filter, 'none', 'Phone entrances must avoid blur');
        await page.locator('#why').scrollIntoViewIfNeeded();
        await page.locator('#pause').click();
        assert.equal(await page.locator('#pause').innerText(), 'Play demo');
        const pauseBox = await page.locator('#pause').boundingBox();
        assert.ok(pauseBox.height >= 44, 'Pause control must be touch sized');
        await page.locator('.fcard.star').scrollIntoViewIfNeeded();
        const expiry = await page.locator('.is-exp').boundingBox();
        const linkText = await page.locator('.is-lt').boundingBox();
        assert.ok(expiry.width <= 100, 'Expiry choice should be a compact segmented control');
        assert.ok(Math.abs(expiry.y + expiry.height / 2 - linkText.y - linkText.height / 2) < 2, 'Link and expiry should share one compact row');
        assert.ok((await page.locator('.is-shot').first().boundingBox()).height <= 72, 'Phone previews should not dominate the sharing demo');
        await page.locator('.is-exp [data-h="1"]').click();
        assert.equal(await page.locator('.is-exp [data-h="1"]').getAttribute('aria-pressed'), 'true');
        assert.ok((await page.locator('.is-exp [data-h="1"]').boundingBox()).height >= 44);
        assert.ok(await page.locator('.fcard.star h3').evaluate((e) => parseFloat(getComputedStyle(e).fontSize) <= 32), 'Phone feature title should match the compact type scale');
        for (const panel of await page.locator('.is-stage, .is-pc, .is-act .btn').all()) {
          assert.ok(await panel.evaluate((e) => e.scrollWidth <= e.clientWidth + 1), 'Sharing panels and controls must fit without clipped content');
        }
        const discord = await page.getByRole('link', { name: 'Join the BuffrX Discord', exact: true }).boundingBox();
        assert.ok(discord.width >= 44 && discord.height >= 44, 'Navigation icons need 44px touch targets');
        await page.getByRole('button', { name: 'Make a link', exact: true }).click();
        await page.waitForFunction(() => document.querySelector('.is-link [data-k="lc"]').textContent === 'copied');
        assert.ok(await page.locator('.is-act [data-k="go"]').evaluate((e) => {
          const range = document.createRange();
          range.selectNodeContents(e);
          const label = range.getBoundingClientRect(), button = e.getBoundingClientRect();
          return label.left >= button.left && label.right <= button.right && label.bottom <= button.bottom;
        }), 'Long sharing action must wrap inside its button');
        await page.getByRole('button', { name: 'Friend clicks the link', exact: true }).click();
        await page.waitForFunction(() => document.querySelector('.fcard.star .demo').dataset.state === 'done');
        await page.locator('.lib-f select').selectOption('cs2');
        assert.equal(await page.locator('.lib .lclip').count(), 2);
        await page.locator('.lib .lclip').first().click();
        await page.getByRole('button', { name: 'Copy', exact: true }).click();
        assert.match(await page.locator('.bcol .demo.top .status').innerText(), /is copied/);
        const handle = page.getByRole('slider', { name: 'Start of the part you keep' });
        await handle.press('ArrowRight');
        assert.equal(await handle.getAttribute('aria-valuenow'), '7');
        const box = await handle.boundingBox();
        const touch = await page.context().newCDPSession(page);
        const x = box.x + box.width / 2, y = box.y + box.height / 2;
        await touch.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y }] });
        for (let dx = 10; dx <= 60; dx += 10) {
          await touch.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: x + dx, y }] });
        }
        await touch.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
        assert.ok(Number(await handle.getAttribute('aria-valuenow')) > 7, 'Touch dragging must still trim the clip');
        await page.locator('#compare').scrollIntoViewIfNeeded();
        assert.equal(await page.locator('#cmp thead').isVisible(), true, 'Compact comparison needs shared column labels');
        const table = await page.locator('#cmp').boundingBox();
        assert.ok(table.height < 720 && table.width <= width - 32, 'Comparison must fit without a long stack of cards');
        const cells = await page.locator('#cmp tbody tr').first().locator('th, td').evaluateAll((els) => els.map((e) => e.getBoundingClientRect().top));
        assert.ok(Math.max(...cells) - Math.min(...cells) < 2, 'Feature and answers should share one row');
        await page.locator('#chips [data-id="obs"]').click();
        assert.match(await page.locator('#cmp thead').innerText(), /OBS Studio/);
        assert.equal(await page.locator('#cmp tbody tr').count(), 10);
        assert.equal(await page.locator('#compare .legend').isVisible(), true);
        await page.locator('#faq').scrollIntoViewIfNeeded();
        const lastQuestion = page.locator('#qa .q:visible').last();
        assert.equal(await lastQuestion.isVisible(), true, 'Essential FAQs remain reachable on phones');
        await lastQuestion.locator('button').click();
        assert.equal(await lastQuestion.locator('button').getAttribute('aria-expanded'), 'true');
      }
      assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1), 'Page must not overflow horizontally');
      assert.deepEqual(errors, []);
    } finally {
      await browser.close();
    }
  });
}

test('phone navigation folds and returns with a transition', async () => {
  const browser = await chromium.launch({ executablePath: chromium.executablePath() });
  try {
    const page = await browser.newPage({ viewport: { width: 390, height: 844 }, hasTouch: true });
    await page.goto(url);
    await page.evaluate(() => scrollTo(0, 400));
    await page.waitForFunction(() => document.querySelector('#nav').classList.contains('min'));
    assert.notEqual(await page.locator('#tabs').evaluate((e) => getComputedStyle(e).display), 'none', 'Tab row must stay rendered during collapse');
    assert.notEqual(await page.locator('#tabs').evaluate((e) => getComputedStyle(e).transitionDuration), '0s');
    await page.waitForFunction(() => getComputedStyle(document.querySelector('#tabs')).visibility === 'hidden');
    await page.evaluate(() => scrollTo(0, 0));
    await page.waitForFunction(() => !document.querySelector('#nav').classList.contains('min'));
    await page.waitForFunction(() => getComputedStyle(document.querySelector('#tabs')).opacity === '1');
    assert.equal(await page.locator('#tabs').isVisible(), true);
  } finally {
    await browser.close();
  }
});

test('reduced motion: phone content stays visible without entrance transitions', async () => {
  const browser = await chromium.launch({ executablePath: chromium.executablePath() });
  try {
    const page = await browser.newPage({ viewport: { width: 390, height: 844 }, hasTouch: true, reducedMotion: 'reduce' });
    await page.goto(url);
    await page.waitForFunction(() => document.querySelectorAll('.fcard').length >= 3);
    const styles = await page.locator('#features .sec-head h2').evaluate((e) => {
      const s = getComputedStyle(e);
      return { opacity: s.opacity, translate: s.translate, duration: s.transitionDuration, clip: s.clipPath };
    });
    assert.equal(styles.opacity, '1');
    assert.equal(styles.translate, 'none');
    assert.equal(styles.duration, '0s');
    assert.equal(styles.clip, 'none');
  } finally {
    await browser.close();
  }
});
