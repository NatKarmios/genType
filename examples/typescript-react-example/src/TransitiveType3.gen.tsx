/* TypeScript file generated from TransitiveType3.re by genType. */
/* eslint-disable import/first */


// tslint:disable-next-line:no-var-requires
const TransitiveType3BS = require('./TransitiveType3.bs');

// tslint:disable-next-line:interface-over-type-literal
export type t3 = { readonly i: number; readonly s: string };

export const convertT3: (_1:t3) => t3 = function (Arg1: any) {
  const result = TransitiveType3BS.convertT3([Arg1.i, Arg1.s]);
  return {i:result[0], s:result[1]}
};
