'use strict';
// Postgres pool for Tiger Data. Tiger signs service certificates with its own CA
// (ca.timescale.com), so we verify against that CA instead of turning verification off.
const fs = require('fs');
const path = require('path');
const { Pool } = require('pg');

function makePool(url = process.env.DATABASE_URL, opts = {}) {
  if (!url) throw new Error('DATABASE_URL is not set (see .env.example)');
  const u = new URL(url);
  u.searchParams.delete('sslmode');           // ssl is configured explicitly below
  return new Pool({
    connectionString: u.toString(),
    ssl: { ca: fs.readFileSync(path.join(__dirname, 'tiger-ca.pem'), 'utf8'), servername: u.hostname },
    max: 5,
    ...opts,
  });
}

module.exports = { makePool };
