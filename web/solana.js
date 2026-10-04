'use strict';
// Crowd Hero: a 0-decimal token on Solana devnet, minted once per resolved SOS.
// The wallet and mint address live in .keys/ (git-ignored). Devnet SOL is free.
const fs = require('fs');
const path = require('path');
const {
  Connection, Keypair, PublicKey, Transaction, TransactionInstruction, sendAndConfirmTransaction,
} = require('@solana/web3.js');
const {
  createMint, getOrCreateAssociatedTokenAccount, createMintToInstruction, TOKEN_PROGRAM_ID,
} = require('@solana/spl-token');

const KEYS = path.join(__dirname, '..', '.keys');
const WALLET_FILE = path.join(KEYS, 'solana.json');
const MINT_FILE = path.join(KEYS, 'mint.txt');
const MEMO = new PublicKey('MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr');
const DEVNET = 'https://api.devnet.solana.com';

function loadWallet() {
  fs.mkdirSync(KEYS, { recursive: true });
  if (fs.existsSync(WALLET_FILE)) {
    return Keypair.fromSecretKey(Uint8Array.from(JSON.parse(fs.readFileSync(WALLET_FILE, 'utf8'))));
  }
  const kp = Keypair.generate();
  fs.writeFileSync(WALLET_FILE, JSON.stringify(Array.from(kp.secretKey)), { mode: 0o600 });
  return kp;
}

async function ensureFunds(connection, pubkey) {
  let lamports = await connection.getBalance(pubkey);
  if (lamports > 20_000_000) return lamports;
  try {
    const sig = await connection.requestAirdrop(pubkey, 500_000_000);
    const latest = await connection.getLatestBlockhash();
    await connection.confirmTransaction({ signature: sig, ...latest }, 'confirmed');
  } catch (err) {
    lamports = await connection.getBalance(pubkey).catch(() => 0);
    if (lamports > 20_000_000) return lamports;
    throw new Error(`Devnet faucet is dry from this network. Request devnet SOL for ${pubkey.toBase58()} at https://faucet.solana.com, then award the badge again.`);
  }
  return connection.getBalance(pubkey);
}

async function ensureMint(connection, payer) {
  if (fs.existsSync(MINT_FILE)) return new PublicKey(fs.readFileSync(MINT_FILE, 'utf8').trim());
  const mint = await createMint(connection, payer, payer.publicKey, null, 0);
  fs.writeFileSync(MINT_FILE, mint.toBase58());
  return mint;
}

async function award({ bracelet, friend, note }) {
  const payer = loadWallet();
  const connection = new Connection(DEVNET, 'confirmed');
  await ensureFunds(connection, payer.publicKey);
  const mint = await ensureMint(connection, payer);
  const ata = await getOrCreateAssociatedTokenAccount(connection, payer, mint, payer.publicKey);
  const line = (note || `Crowd Hero: ${bracelet || 'a bracelet'} helped ${friend || 'a friend'}`).slice(0, 180);
  const tx = new Transaction().add(
    createMintToInstruction(mint, ata.address, payer.publicKey, 1, [], TOKEN_PROGRAM_ID),
    new TransactionInstruction({ keys: [], programId: MEMO, data: Buffer.from(line) })
  );
  const signature = await sendAndConfirmTransaction(connection, tx, [payer]);
  return {
    signature,
    mint: mint.toBase58(),
    wallet: payer.publicKey.toBase58(),
    explorer: `https://explorer.solana.com/tx/${signature}?cluster=devnet`,
    note: line,
  };
}

function status() {
  let wallet = null;
  let mint = null;
  try { wallet = loadWallet().publicKey.toBase58(); } catch (e) { /* first call creates it */ }
  if (fs.existsSync(MINT_FILE)) mint = fs.readFileSync(MINT_FILE, 'utf8').trim();
  return { cluster: 'devnet', wallet, mint };
}

module.exports = { award, status };
