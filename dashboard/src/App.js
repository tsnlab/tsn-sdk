import React, { Component } from 'react';

import { Nic } from './Config';
import './App.css';

const API_BASE_URL = (
  (process.env.NODE_ENV === 'development') ?
  'http://localhost:8000/api' :
  '/api'
);


class App extends Component {
  constructor(props) {
    super(props);
    this.state = {
      config: { nics: {} },
      ifnames: [],
      currentIfname: '',
      initialised: false,
    };
  }

  async componentDidMount() {
    const ifnames = fetch(`${API_BASE_URL}/ifnames`).then(res => res.json());
    const config = fetch(`${API_BASE_URL}/config`).then(res => res.json());

    console.debug(`Using ${API_BASE_URL}`);

    Promise.all([ifnames, config]).then(([ifnames, config]) => {
      this.setState({
        ifnames,
        config,
        initialised: true,
      });
    });
  }

  setCurrentIfname(ifname) {
    console.log(ifname);
    this.setState({
      currentIfname: ifname,
    });
  }

  updateNic(nicConfig) {
    const currentIfname = this.state.currentIfname;
    const { config } = this.state;
    config.nics[currentIfname] = nicConfig;

    this.setState({ config });
  }

  render() {
    const { config, ifnames, currentIfname } = this.state;

    if (!this.state.initialised) {
      return (<div>Loading...</div>);
    }

    const nic = ifnames.includes(currentIfname)
      ? <Nic
        key={ currentIfname }
        ifname={ currentIfname }
        update={ this.updateNic }
        config={ config.nics[currentIfname] || {} } />
      : null;

    console.log(config.nics);

    return (
      <div className="App">
        <div className="nics-menu">
          {ifnames.map((ifname) => {
            return (
              <div className="nic" key={ifname} onClick={() => this.setCurrentIfname(ifname)}>
                {ifname}
              </div>
            );
          })}
        </div>

        { nic }
      </div>
    );
  }
}

export default App;
