module.exports = {
  testDir: './test',
  testMatch: /.*\.pw\.js/,
  timeout: 30000,
  use: {
    headless: true,
    viewport: { width: 1280, height: 900 }
  }
}
