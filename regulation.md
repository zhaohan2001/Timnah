# Conventions

## Types

Use u8 for all depth values (0-48).

Use u32 for indexes, with exceptions where needed.

## Module boundaries

Single responsibility: Each module should do one thing, making future changes easy to locate.

Hide details: Keep modules loosely coupled and their interfaces concise and efficient.

## Naming abbreviations

t == tier
en = enable
gdep = globalDepth
ldep = localDepth
