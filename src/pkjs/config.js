module.exports = [
  {
    type: 'heading',
    defaultValue: 'WeatherDial'
  },
  {
    type: 'section',
    items: [
      {
        type: 'heading',
        defaultValue: 'Location'
      },
      {
        type: 'input',
        messageKey: 'LATITUDE',
        label: 'Latitude',
        attributes: { type: 'number', step: 'any', placeholder: 'e.g. 52.52' }
      },
      {
        type: 'input',
        messageKey: 'LONGITUDE',
        label: 'Longitude',
        attributes: { type: 'number', step: 'any', placeholder: 'e.g. 13.40' }
      },
      {
        type: 'text',
        defaultValue: 'Leave both blank to use your phone\'s location.'
      }
    ]
  },
  {
    type: 'section',
    items: [
      {
        type: 'heading',
        defaultValue: 'Appearance'
      },
      {
        type: 'select',
        messageKey: 'THEME',
        defaultValue: '1',
        label: 'Theme',
        options: [
          { label: 'Light', value: '0' },
          { label: 'Dark', value: '1' },
          { label: 'Blue & Yellow', value: '2' },
          { label: 'Black & White', value: '3' }
        ]
      }
    ]
  },
  {
    type: 'submit',
    defaultValue: 'Save'
  },
  {
    type: 'text',
    defaultValue: 'Weather by <a href="https://open-meteo.com/">Open-Meteo.com</a>'
  }
];
