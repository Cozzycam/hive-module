// Plant-themed name wordlist — matches firmware/include/names.h exactly (200 entries)
const WORDLIST = [
  'Acorn','Alder','Almond','Amber','Anise','Apple','Apricot','Arbutus','Arnica','Ash',
  'Aspen','Aster','Azalea','Balm','Balsam','Barley','Basil','Bay','Beech','Belladonna',
  'Bergamot','Birch','Bistort','Blackthorn','Bladderwort','Blossom','Bluebell','Bog','Borage','Bracken',
  'Bramble','Briar','Broom','Bryony','Buckthorn','Bulrush','Burdock','Bur','Buttercup','Calendula',
  'Camas','Camomile','Campion','Caraway','Cardamom','Catkin','Cedar','Celandine','Chervil','Chestnut',
  'Chicory','Cinnamon','Clary','Cleavers','Clematis','Clover','Coltsfoot','Comfrey','Coriander','Corydalis',
  'Cotton','Cowslip','Cranberry','Crocus','Cumin','Currant','Cypress','Daffodil','Dahlia','Daisy',
  'Damson','Dandelion','Dewberry','Dill','Dock','Dogwood','Echinacea','Elder','Elm','Endive',
  'Eucalyptus','Evening','Fennel','Fern','Feverfew','Fig','Filbert','Flax','Forsythia','Foxglove',
  'Foxtail','Frangipani','Furze','Galangal','Gardenia','Garlic','Gentian','Ginger','Ginseng','Goldenrod',
  'Gorse','Hazel','Heather','Hellebore','Hemlock','Hemp','Henbane','Hibiscus','Holly','Hollyhock',
  'Honeysuckle','Hornbeam','Horsetail','Hyssop','Iris','Ivy','Jasmine','Jonquil','Juniper','Kelp',
  'Knapweed','Lacebark','Larch','Larkspur','Laurel','Lavender','Leek','Lemon','Lichen','Lilac',
  'Lily','Lime','Linden','Liverwort','Lobelia','Lotus','Lovage','Lupin','Mace','Madder',
  'Maidenhair','Mallow','Mandrake','Maple','Marigold','Marjoram','Marsh','Meadowsweet','Mint','Mistletoe',
  'Moss','Mugwort','Mulberry','Mullein','Mustard','Myrrh','Myrtle','Nasturtium','Nettle','Oak',
  'Oats','Olive','Orchid','Oregano','Pansy','Papyrus','Parsley','Pennyroyal','Peony','Periwinkle',
  'Pine','Plantain','Plum','Poppy','Primrose','Quill','Quince','Ragwort','Reed','Rose',
  'Rosemary','Rowan','Rue','Rush','Saffron','Sage','Sandalwood','Sassafras','Savory','Saxifrage',
  'Sedge','Sloe','Sorrel','Speedwell','Spruce','Tansy','Tarragon','Teasel','Thistle','Thrift',
];

export function nameFromId(id: number): string {
  const idx = id % 200;
  const suffix = Math.floor(id / 200);
  const name = WORDLIST[idx] || 'Worker';
  if (suffix === 0) return name;
  return `${name}_${suffix + 1}`;
}
