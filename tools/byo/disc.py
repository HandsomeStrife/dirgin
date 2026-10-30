#!/usr/bin/env python3
"""disc.py -- check a Dirge of Cerberus disc image and pull the two files the build needs.

Bring-your-own-disc helper for tools/byo/build_web.sh. Reads the ISO 9660 image directly
(no mounting, no 7-Zip), so it needs nothing but Python 3.

  python3 -I tools/byo/disc.py check   <image.iso>            disc id + file hashes
  python3 -I tools/byo/disc.py extract <image.iso> <workdir>  FILELIST.BIN + KEL.DAT -> workdir
  python3 -I tools/byo/disc.py verify  <noPath dir>           the unpacked inputs the build reads
  python3 -I tools/byo/disc.py isohash <image.iso>            sha256 of the whole image (slow)

Supported disc: PAL SLES-54185, "Dirge of Cerberus: Final Fantasy VII" (Europe, Australia)
(En,Fr,De,Es,It). The check is on the files inside the image (boot id + SHA-256 of
SYSTEM.CNF, the ELF, FILELIST.BIN and KEL.DAT), not on the image as a whole, so a rip
that differs only in padding still passes. The whole-image hash of the reference image is
in REFERENCE_ISO_SHA256 (Redump-style rip, 4,188,471,296 bytes) for comparison.

Everything this writes is game data: keep it in gitignored places (the build script uses
<repo>/extract/, which .gitignore excludes). Exit status: 0 ok, 1 wrong/damaged disc, 2 usage.
"""
import hashlib, os, struct, sys

SECTOR = 2048
BOOT_ID = 'SLES_541.85'
REFERENCE_ISO_SHA256 = '0b29c6f7947fa6fa3fa1c363fe224856d89b1451c0a561ff63dc7709b39b6e3c'
REFERENCE_ISO_SIZE = 4188471296
# SHA-256 of the files inside the supported disc (hashes are not game data).
KNOWN = {
    'SYSTEM.CNF': '2a524ce9bb4d48258721b92e54727d2d32af52a89db48b948304760e650bfb25',
    'SLES_541.85': 'd51e59218891b61db4438824e04dcb8e19fe8468c7e034b2375856f771c21012',
    'FILELIST.BIN': '6ec5b671f107df1249305a52326a6e8c5c737ba675b66164153dd8858655b850',
    'E419C51B/KEL.DAT': '7ce51b07f2e218a1ba734c379276ec08a73197d3db337158c897cca35303d9cc',
}
# The unpacked files (extract/_KEL/noPath) the web build reads, with their SHA-256: every
# file whose access time moved during a build of the reference disc in a fresh clone
# (2026-10-26). Add new inputs here when an exporter starts reading another file.
INPUTS = {
    '06800001.bin': 'a16d8406cfa107c3517c24642c2c156ffcb378a3d7153f21e70912333cc10eba',
    '06800011.bin': 'dff66ff1ddcacaef9c64095b55b2602b7d90f7aa1495394f11dd9bcfd789893e',
    '06800012.bin': '08e3894c65fc2097725c98d6979f7e659172e3a569cc5f1cf341a94efcf1d50f',
    '06800111.bin': '58e30dc823fdea1a81f825850dc787074d5cb333a4a5ac56f34d74d19e74b482',
    '06800112.bin': 'c39aebf1d1450cf721f793c63d308ff9f38e28992f76f3a67fc3b08fff15f1ad',
    '06800211.bin': '060f279ef8fae8c954d3dab03490bbb77b00c69f28747202f8cc6aabce62aae1',
    '06800212.bin': 'ecdbf717d0e10319e03eae4047732a71749f34cf600fdaa5cbe34616ed4a89ad',
    '06800311.bin': '87a66ff93de6faa9f7e0dff875510ed19905043367f1ed96d2b8bcb128131460',
    '06800312.bin': '82ef40f5e7755f90d1db7ca625980da6d747fe495c3c4b3bfd081123067a495c',
    '06800411.bin': 'c6aabf8f6310502cdb9f04f3f92fcf8add5e5da3fb6353795971ba1e894e8a22',
    '06800412.bin': 'bc57d742e26ce16a7d7f7e750eeb758c7c49c6a7297fa14257cdb2015f72aa29',
    '06800511.bin': '86661c16a42ef92363612ae7660f795b3bb53b1362e7a05c8c3c7e1135ea7058',
    '06800512.bin': '59d554f8c73a43a179b572255aed22696e6774ede9c7cbf963b77ff4384850c1',
    '06800611.bin': 'd61acd1595348cad8f99312eb6896f89d216e61bc21dbbf81bda2254b523e9a7',
    '06800612.bin': '8f60a2425ed5d4b9d5cb41a324cba52b7e784e28cdb243b3d7a6abde848518ab',
    '06800711.bin': '9e640de5b05d466a9bf072b4f67dde204f2a1de749f921301b2bc0b0db37d00c',
    '06800712.bin': '69ca2c91199e4cf47be39da239472af65336941dfb8a3c8dc268b8e3dc66a532',
    '06800811.bin': '8d62e8eeb2a1635be802cfdcfbb80689f3699fb7ac147e80a2009336d221fdd8',
    '06800812.bin': 'db5dfffe43cece4903784216993d9955cb3e7deed7931c10842642a7af796973',
    '06800911.bin': '6577c7bd0b888be4301f45ec657da576bd4987788d7cfdb4b44756900554f012',
    '06800912.bin': '053ae5f9ddcc79e4705b44324e9399c5f2e15d944e2fedc85cf29d3e6a2f5f3d',
    '06800a11.bin': 'd877d85bff794817b280b9b5cd88f0e5b016a9fbcea5f1ea9f6070fb13a1a73e',
    '06800a12.bin': 'c3e5237ab032504f81af9803137260415b96f2f502099ccd0099857b3f827030',
    '06800b11.bin': '177fa2d83b940104a3a0f7582eb4227fc180549be6a8563dd829b6529405debe',
    '06800b12.bin': '64644116c1cee296db7a7f24c695a8b1369d798e588565eeef81649ed080ee34',
    '06800c11.bin': '31dae5b7135815658cb4885097672b114aa43014479375639ee4945cda6a90d4',
    '06800c12.bin': '5c5834d36558e1d3debad32519f872ec22f702fbca4af4c200e2c12fbb04a827',
    '0680ff00.bin': '3dbbff24e326965848e8bd1f2a0688cdde4755cb89cab1b5fc203d83c5d0d479',
    '0680ff01.bin': '5e732149e2e312ca68cdee0585f3e836027b7acd2814866fdade2c9efb722a0e',
    '0680ff02.bin': '646bc9044050f8b8362565803222149645508fd71708a19ee6cf8548c585462b',
    '0680ff03.bin': 'f4630c4152c73dfcdafe01327cbffcb755c4f15897819eeaa65b699980d60e0f',
    '0680ff05.bin': '75fe7bc26df6f1fa6f43f5ca761adb4d636aac664002396170f4cf1200d13db0',
    '0680ff07.bin': '76543abae5b0f285e75f008111e370c17c3cbb11a738679501b2796d23c472d6',
    '0680ff08.bin': '1dfdb82c94ffa526043be93f0c4dbb0e73ecc778265f2163881253dc95959011',
    '0680ff09.bin': '021be3080df868f1c50d2b2c50bbcbae489a03555f77f3d64f14048f887415bc',
    '0680ff0a.bin': 'df6c323adf1d0b51cb11070b37456c89df356f79a91fb53ed142ee240f2b095e',
    '0680ff0b.bin': '79fb82fa3f99d273b6a75e343362d717f3c64e10e51be6b337e46a1bc7e7bfd5',
    '0680ff0c.bin': '549f270e58ef176ec0247633ec880a36d7ab0e80cab0be15b8dde36388e6153c',
    '0680ff0e.bin': '51d71a83d4e690ee7f208dd335941d236bd2aaee3b7a77e41ffda6a9ae86513f',
    '0680ff0f.bin': '69fbfb900e50fc90d0de1adab7d4dd5b01c3ec4d47bd63de4c4bd865517a94cf',
    '0680ff10.bin': 'c29dbbfa5101cddb635b19c9d6296e4cb6664aa4eec3ab90138a68af1b2362ac',
    '0680ff11.bin': '6862e1bc7c49b83214e307e856f2c61bde44c0ee145ab1081f92d203cab859db',
    '0680ff12.bin': 'e24f35c80842810f33d9167284a4b5e69c4ba059865f089d1560f7119cafebea',
    '0680ff13.bin': '50729b0c051d6d96962272637a039d6f00d485f145eb011347522af5b1f78620',
    '0680ff14.bin': '9e3e2d627b0eea3775f6ea4c05e9ad02ad9f9b1969b0e6a0f16bd57bfccf701c',
    '0680ff15.bin': 'c7bd084294dcc783db235a163c76efe572058df0c6cd04ca67418dd8b5aa948b',
    '0680ff16.bin': 'f26eb458aba982150ac10a0cb0a90f28c5ff36ea08976692a3c266218146c23e',
    '0680ff17.bin': '1c0909328e79d1edbe7e97d3ba5c846c85474f8028a7edad329a5e3f9e872ff1',
    '0680ff18.bin': 'ad102dca136f6d5b46b377b8ccde18165410adb7302e7dfebec624564620278f',
    '0680ff19.bin': '0dd4dd037e0d3447adb90fb3611aac4237498682c4c01be3604a1330132bd8eb',
    '0680ff1a.bin': 'e89d657fbfe7166eddbde52ff8c158a0821a161f68e9f03df7df064aabd8f881',
    '0680ff1b.bin': 'aeadb843c424d22a5d62f7c99e958c8a4eb5d9e6e8d95232dd6c6f109c6a4714',
    '0680ff1c.bin': '30b35a9e8f0f4a391f4dca37d9dcb66004c625ab19f1af86377e364d166c0cd9',
    '0680ff1d.bin': '52b6e839af2fc8fbced65eecbdbcfa26834a39e309f53d4ef44b62e69f1b512c',
    '0680ff1e.bin': '96ec8ab6a0faf45aef9e8fcd1bfea9fcb34ad51220e9e24834fe7030d4653ba1',
    '0680ff1f.bin': 'a71dacc5875d8ec60ad5b9f70e7d19cfef58d7406750655ac6d333537aa9ccd5',
    '0680ff20.bin': '87408d92d91d891cb8e2643fa775c9b33a5039a7a301ec9575955a21f83b7ad1',
    '0680ff21.bin': '70f2665ea0e73113cfa7a889864698ecdab45133724ffb6372c85a53ede5f9a1',
    '0680ff22.bin': 'd116f44bbf223edc29d01bfe68656b72e7ffda56a9cf3a65313edb79a747ea1f',
    '0680ff23.bin': '6aa11898349abaabf939b88d1b6d2b60c26e9f7f38a875b475114383a7c6608f',
    '0680ff24.bin': 'ee05003e29d28854ad7ded803507f8e6cdd658f000d6de738c6720700438f5ae',
    '0680ff25.bin': 'e5ca81be8cd69d0932afa3c9d3aafa9575a474ee7fbb38441d1965b3d87daa68',
    '0680ff27.bin': '74e8ca81068b2d88aa1749e5f0466f745fc8cacdd879cf71af5a9c71c39dbf14',
    '0680ff28.bin': '6d0baac0a27c33b34f5340836ce0c9a93f69501d720ea58786a0682e33226626',
    '0680ff29.bin': 'f15a2ef79e1beb2156b744b322c5624e6e386a288eec9dd0ba2f7264eb7f50a2',
    '0680ff2a.bin': '1fc2ad7403876f165116aaef13002dc5533071786833bf745a3bc4aa4f8f29be',
    '0680ff2b.bin': '6e820d3c09dd409365637e6c5d81cba05ebae2eb1b1169fce7b96395afb36c9f',
    '0680ff2c.bin': '9ccd6a5aa38f84bd5603120353123b5a0563fe1c4d28537fd4dbcafee500f028',
    '0680ff2d.bin': '236fe6f61b3a57c402d8d6f32a260684a419ef461030ce6fbd71f631e0f5f9e3',
    '0680ff2e.bin': '2f0ef7c6bc70340a698a663e3bf131c72be041fd6a96eba481e3f0cd019d0622',
    '0680ff2f.bin': 'e1b65f596721a9ce5e77513fedc1a1040db5ad142a5435029aded73db063f777',
    '0680ff30.bin': 'e343961f3b6b6cce8d2f6e1d791d832a1da93097d61b37e9a66085f05d79d988',
    '0680ff31.bin': 'ef4f2e323bf116797e05e4532e1f073a9b31d1c4508b5b91b198e95bee00fecf',
    '0680ff32.bin': '30a183a2b00c19137ad6b6b22ddaa500496a730ca9f77d8e5c476ad2cb57840a',
    '0680ff33.bin': 'b2526832cab2a3450a663b4da86790421f1ff494581990b1e9ca28446d5b5c11',
    '0680ff34.bin': '2fd57e837e96b849a5e13313bf37b9c7033d7d69489683ef10c24dc49a7e315b',
    '0680ff35.bin': 'feb12fb05d6aa292e46f0d8a6408d7ac2169c2d51053f758e89df59a0bd9f1e8',
    '0680ff36.bin': '1de4a9c62b4922bbe5f72955408b030709bd901e81491c6e68be168143bb6693',
    '0680ff37.bin': 'd527e014c11c3c94e9ae2e60fa31c5edc07d461379d436e470d52fae54541909',
    '0680ff38.bin': 'f5bc67388ef62807fba49b2a85894c77974d2ef0db28065e81ade43863411221',
    '0680ff39.bin': '5f15a2c323bcb7ebcb42af395cc8c9b07950cffa6b752d7614978ebefb2d2318',
    '0680ff3a.bin': '3b7b1018f247101742f7b8a884ae7c1d41d406d72f344e252cb8a10251b8f0bb',
    '0680ff3b.bin': 'ad31993dc5c22d7726310032b69181b6b451a57f50c3e5f675e1248b5328b7b2',
    '0680ff3c.bin': '3208c7442a2682f313a462823da693bdc9766de13d63721d66fb347ca8acc40c',
    '0680ff3e.bin': '3fce57395c394f926bfcfb4ccc10b7db556bf8ae4e4b5edc01076791df904348',
    '0680ff3f.bin': '40c6c64ea05b53390b65c371bc2ef408b872ea6b59e702aa78869e9c66b3149b',
    '0680ff40.bin': '37c8e6231e13e6051417be654437533f9ade23098ac30d0948c6397c4f32098c',
    '0680ff41.bin': '27687667f0d7570e804c65013cf8898a65527b4e60203aac887999b77f2faaf4',
    '0680ff42.bin': '092e901ae1c24dadb321daf01c7407daf4bf7b80445620e6d0cc9a7b6ce7eed3',
    '0680ff43.bin': '21d2221417a836d3980e22676b722511aa642c6346e2f06ec11731302893643c',
    '0680ff44.bin': 'df8fb32231037ee5c5bb127d5678ce4c49c1fb53fd45c8763ea1949a412e3ab5',
    '0680ff45.bin': '13d2b520b75c41ba01aec8abb6626d91bab75b5a940728c946a09067e128b65e',
    '0680ff46.bin': '61fcfbc6198af13ec3d1b2ac30ccd7d42cc2501b84039ee71611dcb2fbfef32f',
    '0680ff47.bin': '35dca74f25270d13525fb94f42e65a7894427271d6d0baf61aabe38d40f660b6',
    '0680ff48.bin': 'fad2b877c13dbfb01a567c156437043682439949e9ad76640a13343f63e62575',
    '0680ff49.bin': 'c95c71711c3546306e083450b0a232e42562f0fe29774f52aa2ff2312c1fcd58',
    '0680ff4a.bin': '60308ad3b2a3b6f6ff74e10e755e44f48f4905d89593b1dc9b4dbeb08292397c',
    '0680ff4b.bin': 'f8af7a1c58daa642a075ed60def272864c24c6ec17383376da6c4cacabc62dce',
    '0680ff4c.bin': '4f0f556486dd4bde0e12a8bff2ed0ecc4a233fd72d1b89ec9740c5f6a4a98459',
    '0680ff4d.bin': 'f57d5f49e6b856087da2f702265f614071325bcdc27d204b93dd82ff2a51640f',
    '0680ff4e.bin': '2ea330c228af717d0048a8cee8becaab9bfa7e6abc9c048b026b8e575f4bdc70',
    '0680ff4f.bin': '0e35bfd205571c843bae32e9b911c42ba887fe2c02ea153b9e56c8de2c6b4b28',
    '0680ff50.bin': 'fc2cd3960a306c78f46e61aa39fe9aeb06f19c006fe104115d6ba1f0520633e1',
    '0680ff51.bin': 'b62f1a237b695003ed4e9e52266cb2f6be11cacd987feae4403a55efc70244ec',
    '0680ff52.bin': '290451c3df2401ac00bf23e432fb398ad7e427de8c7f7bb5946b76f03dc82f92',
    '0680ff53.bin': 'd05374b8a441717c680abaa5653cc93576c52327ddf03988940b0430cacd0f47',
    '0680ff54.bin': '056071d4749f4ff78f897ea172124b60e3e5c8917ea4980bfbea461bc5c1077e',
    '0680ff55.bin': '0a9747cb8bcff95cc680ab7de9ea4ff5f6085e1f73f376752129d3321deb3c2f',
    '0680ff56.bin': 'a71eb538d62ea9498af5f454ce4eab8efbe748f866c2f9e9318856b054bc6f4f',
    '0680ff57.bin': '6bf25adf5093c1b5e45f859d00ce0d5a529ca1b7da72d31c698d001f84654740',
    '0680ff58.bin': 'feba77f6485216aab050747d22b5ba34b14eec10dcb6d53cdd2be523e6af5d81',
    '0680ff59.bin': 'c0723f3a4f1166f641876749b02e3f506d2099019017d8027920adba67134f06',
    '0680ff5a.bin': '0d7cf9f27260452ff8e28bc4d48b763b96dd345ce3993564a4dc2b0e3f370ff9',
    '0680ff5b.bin': '969e2b25debd38e92d98a5eeedfbf626ddb25cd1fdc08b0fa3ba249b885aab80',
    '0680ff5c.bin': 'f977297d626f4373106f789bddcddf70dc8edf81afd50594c700701113f25edc',
    '0680ff5d.bin': '826b598ae4e99cd41e76da93f5ebc0a260e349cd2a2668b1efa176ecec2ace62',
    '0680ff5e.bin': 'aa161ffdb5bd6fd35b2405abf3394b65c0b8795837f949e42f418074d540321e',
    '0680ff5f.bin': '504fe81f6cdbd484b03a60b5acdff128cbf18cb8b04efc12de4d9ef35bbfaea0',
    '0680ff60.bin': '481b41a2fcdda167e9ed28f3a551887f5d8d44c8e934dc52318cab088c037902',
    '0680ff61.bin': '20ba4d799b820493eec1288020508888341604aade0af8baecee668ad6658667',
    '0680ff62.bin': '8aac857e32c9c7bacee3b4c88588621fb5e24b2d1ac4da80c59b29c63cc236b3',
    '0680ff63.bin': 'b4e5a7b745bb9fe2d573ea76037ccab30f372f17da0f9518997e8725c9813761',
    '0680ff64.bin': 'cd3db3a80d697fa0f39f866a3ba8318032af8a88df5cf6d9665f225225cacb9b',
    '0680ff65.bin': '3be7a5f64c40f962bbad01cc1dfa66c0bbdf54ea8c2f855a5acf04ff3bca3419',
    '0680ff66.bin': '8658da9797e2dff4de26977eab8a057d7c455fe59de133da537a7fe4fbf5825d',
    '0680ff67.bin': '46e24e4cc0d5acaf1015ef49117b62566eea7334d37a80f6861ea7f461f4bdc1',
    '0680ff68.bin': '9bdf1c6b8115019072547803be9032b8d6eb9337df2aaa015b7661e5c79ff8f2',
    '0680ff69.bin': 'f5f3696d549919ca7416868acb56c2e8069936906684df63f7069a5d396e0e87',
    '0680ff6a.bin': 'e16a496a52a908946bf37b0a906a8299155318f9deac8ea89ab53c538e0b514a',
    '0680ff6b.bin': 'e74bdb24fb82fcea06a80307716ce4360fe84798314cc92c55b3b75233dcec20',
    '0680ff6c.bin': '527446f94143350a29e827b3303ce036c6267b3163dc14c78560409524fa1639',
    '0680ff6d.bin': '5136cd0e96fb35ddd675efdc19b4f66ccf3f9ff0a21c6f6492fab436bf3ed79e',
    '0680ff6e.bin': '01e4b81987fac2b2bd2d231b8c12c2e85e0a90d6bd9d24003c034a8fa6fb604d',
    '0680ff6f.bin': 'aebd1683a1bfdbff68a909c4cdaaa43de05683ec74afd9725f4f338eee0f7510',
    '0680ff70.bin': 'fe3fe88a1a53373f028b536868f8dd89f019af9504f5f4da4e980eb4e97c4300',
    '0680ff71.bin': '21155980b720839d4292e176d9bd49b59194b93000c5e0f7a0b161d5b9e97629',
    '0680ff72.bin': 'a5732f27a666d88001534eb880fc253f07030704b9e7ad221bbb3408258506d3',
    '0680ff73.bin': 'd201d7d4f5de7e914c57fc11615aa22a1c7441a9f8124f9962d7dbb7f578fd9b',
    '0680ff74.bin': '3075e1ca1bad934b8eded5a027058c093f373233f65dd047d9b60830fbe6093b',
    '0680ff75.bin': 'd385190344be20d5360fa609a0ff8556b5a635ff313ab2a3fd13ae28186f7b68',
    '0680ff76.bin': '34a7c492b796c597c6fcd36ac01f1a650f66fc881a85ec31a03e56de4dab551c',
    '0680ff78.bin': '44c0cdd58c38fcaf095c243df832cd6dc840dd51e64553ce0f48ed19719e23bb',
    '0680ff79.bin': '7f54665055dfe8e10d9243d78c1e06aa9b6bb4c2d04f892ad6cfe9ac22734489',
    '0680ff7a.bin': 'adb0e93aea37f93ec41e22ff9bff1e98cbb6e0e2e674ac1ade67f2a502b708cd',
    '0680ff7b.bin': 'd55a724f0c3348d76178d0e52e5cea82da1e31b281e54f6076548d70f08ce5a1',
    '0680ff7c.bin': '192bdafba24568eb95dab0a2a1de1c66f422e0a1e9bca3c6e1c3a4f2b7fe17fe',
    '0680ff7d.bin': 'da0bb42586cc033c7b68e611905e11376711ec0e69190e131c7fce66266dad97',
    '0680ff7e.bin': '6d571b1f53d74950359cf4092ad257fa7d216809e9bdc20b19c24397a30f4b27',
    '0680ff7f.bin': '34f5f682adf5b4a995859cc7eb7c05573915167973da8ccd8751f71b9fa57d74',
    '0680ff80.bin': '4751e8ca6240417fd75c31e0de3e0009ef373e95d8eeb3b2e63ba4b2f1fc5e67',
    '0680ff81.bin': 'd92f89fea9c5a4679582bc39e35c5d55e48075ccd3772dee0d8a120e1c3fc199',
    '0680ff82.bin': '14778248dab4dd8ec98b0289b1931d542d80df4c13b90fc8caad62334896daf0',
    '0680ff83.bin': 'bc6a0d01887a70645bf9ca1a80788cccf711b5e98ebe9765a1646c638482d33d',
    '0680ff84.bin': '9b65cbc345617f98ee839d883931289c52b033f95c3ce11620d3a9f3e7638951',
    '0680ff85.bin': 'dd68b1fe89e59afee8fbebadc87728bae5a439c437136100c385ae5033ee3592',
    '0680ff86.bin': 'c71f5539c626fc2f09cd3ca9ba3b97e05af9fb0f00e35846958cae6639106ab1',
    '0680ff87.bin': '6a06e6496d2e3bc532b449d5bde50fa7815858129b2cb7852176e9efab07a6e6',
    '0680ff88.bin': '064c6f138e5ce060af63f2c0ea4d582f21d391a6bb9ac2f0b45393d8a42a6e36',
    '0680ff89.bin': 'dd16ee03f5bf84a2c3c6bbb5de7a1ee71020f5bd36ec7b1c82268397fd7d53ef',
    '07700000.bin': '0923a631733943ea75851f0720ce334933a4de56616bc8a1f7a23e7c6cd531dc',
    '07700001.bin': '40b4a895ff0e7eddea56b748e35bd267072d458b68407f12b26a792bacec9ea6',
    '07700002.bin': '01719bb578c8881ff45be565697361484625cc761347708e890a41082c7c0ecf',
    '0770000b.bin': '22859fa287637c24d11e5e2a6cc46919639ce41386e9b4665f0b27ba7d1458fc',
    '0770000c.bin': 'c5124ebc3ae299f3fd19332bc6fcd48f41d5d2f35f9eed259866ec2d11eeef97',
    '0770000d.bin': '721896d8e570fdf2b77d2ea46cdca5a9a3e30c431c031ff8e7d9e36f5bc97197',
    '0770000f.bin': 'c5f110273517774a0636a55b7f50112f98f721b6e5606d77804185fb73f44a0f',
    '07700011.bin': '8272529347f0eec99ab7884b88a0119975a9671e159fdda15b187443facfe206',
    '076506c1.bin': 'a4e4e9d70dbf65d279acb18fdcba34717e4006afdc4c42c46a9dc31e8c9b9f73',   # e027 model (export_web_enemy)
    '076506c2.bin': 'b69e6647ad51724335f325eec6b7ff97c019bde8dfa8868e7c4ec51f071c820a',   # e027 textures
    '076506cb.bin': '95430e94db04ee4cd74dd28b991ce8d30540423a827e162b24bdc1f470bac500',   # e027 rifle clips (r_gst)
    '077700c1.bin': '3651ff2ee1a90b9ff2c40de1078d9a1d4d829c31c8edb178a100792c082fed4e',   # w003 rifle model (soldiers)
    '077700c2.bin': 'b3872f96ba1ca583b327c0edcd0279638928356d0acc734f4e2d88fecbac11a5',   # w003 textures
    '07770101.bin': 'abddbc0ed50cdcf956cf807ffde04258e9a21fb0916f1168376384e7e6a3aab9',   # w004 model
    '07770102.bin': '241f2c235d69e0b2f04b132279aff9d0d889527220348da94337b029c8387dfd',   # w004 textures
    '07650781.bin': '268bbdde2a9b6c921cbb6922e16a33e8ebf4be2a44c1f9c2019248b1be188188',   # e030 model (export_web_enemy)
    '07650782.bin': '86a99ed63bd3192997038e0abd995a294b965224ed39233456476ca15c546bb1',   # e030 textures
    '07650780.bin': '1f559386248f785472d796d0bd090da9bf734b96b6c25164b4503b1f41932960',   # e030 clips m000 (st_a)
    '07650c01.bin': 'cf726d599130408de23b88ac5c4f2514e84ebee3bf6e346f9293ec45008b50b4',   # e048 model
    '07650c02.bin': 'dfafefbec2e87e6cbb235a6cc9d36765fdd68cd5974e136214a1f366c1439a67',   # e048 textures
    '07650c0b.bin': 'e0d94acb79ab560ddc0aba450530d2f3fe76039c9f7e8e643278521a9ee45bcb',   # e048 rifle clips m001 (r_gst)
    '07650901.bin': '576353359ef6f903583e98316ed911109746db8b6de38510d6cfd40f0a7a652e',   # e036 model
    '07650902.bin': 'e643b72d44bd27773bb52515652689a3151b6cb5f8d8fa5b3f45360e47a3b3bb',   # e036 textures
    '07650900.bin': '06cc8ec975854e715bd1e88dd9b8d5f110e595249943a8d2b290ef4dd8b64950',   # e036 clips m000 (m_gst)
    '076505c1.bin': '93001dd18f2fa985cdf4e3b9450fc501ddc75a46ca0b8fb469622b49308e9dbd',   # e023 model
    '076505c2.bin': 'da9383366d3e7fa868dd29fc864578768df6d8fce151db46aad0df7e415d5e42',   # e023 textures
    '076505cb.bin': '7d4db9a7df0711a70591419d89999c39375de040debe43f7502f96a8d77523b0',   # e023 clips m001 (idle)
    '07650601.bin': '82a80ce24910de40f55b4a801421593f89fedbd76dc6f75ed78735dd18cc48a9',   # e024 model
    '07650602.bin': '3b36abb7fa20c0f4091ef12a72f51e802670500fa1900d6e3c662e65666d2bc0',   # e024 textures
    '07650600.bin': 'a140a2f1c369a3bb4fed25726533947e8242ddb32e21b9cbbfa2e1c3cf3da8ae',   # e024 clips m000 (idle)
    '07650041.bin': '5cf8aacbbf8180e64596cba5a8f21dd09a5ab841008bcecd6e489cdd55897484',   # e001 model
    '07650042.bin': 'c32c4a27b524a8ed7bff7cb2fe9e809a265dc350f25943963478d46893978a84',   # e001 textures
    '07650040.bin': 'c6fdc70867907cbf2b423e4bd8239f4ab98e69fdd1157eaf1a18aed373bde697',   # e001 clips m000 (n_st)
    '07703201.bin': '9b5570610fb12e2eaf834cec0025c3cc18f7c8a64ba5f92d416d0b78e5d315d9',
    '07703202.bin': '06cfcf6ce98c8e60abb4ba119489c67da2a70429bfd32df3c44acd271cb137c3',
    '07703200.bin': 'bac21ed575aa60e3742de4b0c1c36b787444de612f566b4d4e24f1a842181a75',
    '0770320b.bin': '2514dbc8b17af70e17fd2c837f05a6686353ab33b1d9f7dfbcf23abc9100ba0c',
    '0770320c.bin': '10f217b70bdf8aa813b4c6deed1dd7caacc5472d5ac1acf4b368023900ba6837',
    '0770320d.bin': '53290775212bcc4b41679936478fe94353a33d67aa196bb606fcc443d4e083cd',
    '0770320f.bin': '30ce86dd231938655d31f4318afa9d2829bd579bcec6c9d33b1bf031ceebb799',
    '07703211.bin': '8272529347f0eec99ab7884b88a0119975a9671e159fdda15b187443facfe206',
    '07770580.bin': '7be3083169b919c22f4cfe71cf775bcc44526b12fe9d2f46bc2ef291479c95c7',
    '07770581.bin': 'c9f56a9e30cc3e4676e2d856501ac140d198bed3d3262bb23e41a66c1a6f044a',
    '07770582.bin': '78ede02dcc7662df0e9e3fb1f9b0239b58a72cc46fc9b5fac49fd8bc341428f9',
    # outside noPath (data/bgd): weapon parts for export_web_aim.py
    '../data/bgd/bgd.bin': 'cab126367d9fb61865d03323938cc422a450f15e4ab72de5999f6d2a6e21a9a3',
    # outside noPath (data/etc): the HUD 'menu2' texture (reticle) for export_web_aim.py
    '../data/etc/menudata.bin': 'b851a405451880e46a9f95c4be406b958ac81c44a9ac8de7bd7b8e7dccd74eb6',
}


def die(msg, code=1):
    print('disc.py: ' + msg, file=sys.stderr)
    sys.exit(code)


class Iso:
    """Minimal ISO 9660 reader: primary volume descriptor + directory records."""

    def __init__(self, path):
        self.path = path
        try:
            self.f = open(path, 'rb')
        except OSError as e:
            die(f'cannot open {path}: {e}')
        self.f.seek(16 * SECTOR)
        pvd = self.f.read(SECTOR)
        if len(pvd) < SECTOR or pvd[0] != 1 or pvd[1:6] != b'CD001':
            die(f'{path}: not an ISO 9660 image (no primary volume descriptor). '
                'Use a plain .iso (2048-byte sectors); convert .bin/.cue, .mdf, .nrg or .chd first.')
        self.volume_id = pvd[40:72].decode('ascii', 'replace').strip()
        self.root = self._record(pvd[156:156 + 34])

    @staticmethod
    def _record(r):
        n = r[32]
        return dict(lba=struct.unpack_from('<I', r, 2)[0], size=struct.unpack_from('<I', r, 10)[0],
                    dir=bool(r[25] & 2), name=r[33:33 + n].decode('ascii', 'replace').split(';')[0])

    def listdir(self, ent):
        self.f.seek(ent['lba'] * SECTOR)
        data = self.f.read(ent['size'])
        out, o = [], 0
        while o < len(data):
            ln = data[o]
            if ln == 0:                                # records don't cross sectors
                o = (o // SECTOR + 1) * SECTOR
                continue
            r = self._record(data[o:o + ln])
            if r['name'] not in ('\x00', '\x01'):
                out.append(r)
            o += ln
        return out

    def find(self, path):
        ent = self.root
        for part in path.split('/'):
            hit = [e for e in self.listdir(ent) if e['name'].upper() == part.upper()]
            if not hit:
                return None
            ent = hit[0]
        return ent

    def read(self, ent):
        self.f.seek(ent['lba'] * SECTOR)
        return self.f.read(ent['size'])

    def copy(self, ent, dst):
        """Stream a file out of the image; returns its SHA-256."""
        h = hashlib.sha256(); left = ent['size']
        self.f.seek(ent['lba'] * SECTOR)
        tmp = dst + '.part'
        with open(tmp, 'wb') as o:
            while left:
                b = self.f.read(min(left, 8 << 20))
                if not b:
                    die(f'{self.path}: image is truncated (reading {dst})')
                o.write(b); h.update(b); left -= len(b)
        os.replace(tmp, dst)
        return h.hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(8 << 20), b''):
            h.update(b)
    return h.hexdigest()


def check(iso_path, quiet=False):
    iso = Iso(iso_path)
    cnf = iso.find('SYSTEM.CNF')
    if cnf is None:
        die(f'{iso_path}: no SYSTEM.CNF, so this is not a PS2 game disc')
    cnf_text = iso.read(cnf).decode('ascii', 'replace')
    if BOOT_ID not in cnf_text:
        boot = [l for l in cnf_text.splitlines() if 'BOOT' in l.upper()]
        die(f'{iso_path}: wrong disc. It boots {boot[0].strip() if boot else "?"}; '
            f'the supported disc boots {BOOT_ID} (PAL Europe/Australia). See docs/BROWSER.md.')
    got = {}
    for name in KNOWN:
        if name == 'E419C51B/KEL.DAT':
            continue                                    # 1.5 GB: hashed while extracting
        ent = iso.find(name)
        if ent is None:
            die(f'{iso_path}: {name} is missing; the image is incomplete or damaged')
        got[name] = hashlib.sha256(iso.read(ent)).hexdigest()
    if iso.find('E419C51B/KEL.DAT') is None:
        die(f'{iso_path}: E419C51B/KEL.DAT is missing; the image is incomplete or damaged')
    bad = [n for n, h in got.items() if KNOWN[n] and h != KNOWN[n]]
    if not quiet:
        print(f'disc: {BOOT_ID}, volume "{iso.volume_id}", {os.path.getsize(iso_path):,} bytes')
        for n, h in got.items():
            print(f'  {n:18s} {h}  {"ok" if KNOWN[n] == h else ("MISMATCH" if KNOWN[n] else "(no reference)")}')
    if bad:
        die(f'{iso_path}: {", ".join(bad)} differ from the supported disc (different revision, '
            'or a damaged/patched image). The build only supports the reference files.')
    return iso


def extract(iso_path, work):
    iso = check(iso_path)
    os.makedirs(work, exist_ok=True)
    for name, out in (('FILELIST.BIN', 'FILELIST.BIN'), ('E419C51B/KEL.DAT', 'KEL.DAT')):
        dst = os.path.join(work, out)
        print(f'  copying {name} -> {dst}', flush=True)
        h = iso.copy(iso.find(name), dst)
        want = KNOWN[name]
        if want and h != want:
            os.remove(dst)
            die(f'{name}: SHA-256 {h} does not match the supported disc ({want}). '
                'The image is damaged or a different revision.')
        print(f'  {name:18s} {h}  {"ok" if want == h else "(no reference)"}')


def verify(nopath, quiet=False):
    """Check every input file the build reads. Returns the list of problems."""
    probs = [] if INPUTS else ['no input list in disc.py (INPUTS is empty)']
    for name, want in sorted(INPUTS.items()):
        p = os.path.join(nopath, name)
        if not os.path.isfile(p):
            probs.append(f'missing {name}')
        elif sha256_file(p) != want:
            probs.append(f'hash mismatch {name}')
    if not quiet:
        print(f'inputs: {len(INPUTS) - len(probs)}/{len(INPUTS)} ok' + ''.join('\n  ' + x for x in probs))
    return probs


def main(argv):
    if len(argv) < 2:
        die(__doc__, 2)
    cmd = argv[0]
    if cmd == 'check' and len(argv) == 2:
        check(argv[1])
    elif cmd == 'extract' and len(argv) == 3:
        extract(argv[1], argv[2])
    elif cmd == 'verify' and len(argv) == 2:
        sys.exit(1 if verify(argv[1]) else 0)
    elif cmd == 'isohash' and len(argv) == 2:
        h = sha256_file(argv[1])
        print(h, 'matches the reference image' if h == REFERENCE_ISO_SHA256 else
              'differs from the reference image (fine if `check` passes: padding/rip differences)')
    else:
        die(__doc__, 2)


if __name__ == '__main__':
    main(sys.argv[1:])
