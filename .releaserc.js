const parserOpts = {
  breakingHeaderPattern: /^(\w*)(?:\((.*)\))?!: (.*)$/,
}

module.exports = {
  branches: ['main'],
  plugins: [
    ['@semantic-release/commit-analyzer', { parserOpts }],
    ['@semantic-release/release-notes-generator', { parserOpts }],
    '@semantic-release/github',
  ],
}
