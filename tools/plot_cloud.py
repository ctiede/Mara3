#!/usr/bin/env python3



import argparse
import numpy as np
import h5py
import matplotlib.pyplot as plt



def plot_single_file(filename):
    fig, (axes, cb_axes) = plt.subplots(nrows=2, ncols=4, figsize=[15, 8], gridspec_kw={'height_ratios': [19, 1]})
    h5f = h5py.File(filename, 'r')

    r = h5f['radial_vertices'][...]
    q = h5f['polar_vertices'][...]
    d = h5f['mass_density'][...]
    p = h5f['gas_pressure'][...]
    u = h5f['radial_gamma_beta'][...]
    dL = h5f['radial_energy_flow'][...]
    dO = h5f['solid_angle_at_theta'][...]
    r /= r[0]

    R, Q = np.meshgrid(r, q)
    X = np.log10(R) * np.cos(Q)
    Y = np.log10(R) * np.sin(Q)

    m0 = axes[0].pcolormesh(Y, X, np.log10(d.T))
    m1 = axes[1].pcolormesh(Y, X, np.log10(p.T))
    m2 = axes[2].pcolormesh(Y, X, np.log10(u.T), vmin=-3, vmax=2)
    m3 = axes[3].pcolormesh(Y, X, np.log10(dL / dO).T)

    axes[0].set_title(r'$\log_{10}(\rho)$')
    axes[1].set_title(r'$\log_{10}(p)$')
    axes[2].set_title(r'$\log_{10}(\Gamma \beta_r)$')
    axes[3].set_title(r'Luminosity $dL / d\Omega$')

    for m, cax in zip([m0, m1, m2, m3], cb_axes):
        fig.colorbar(m, cax=cax, orientation='horizontal')

    for ax in axes:
        ax.set_aspect('equal')
        ax.set_xticks([])
        if ax is not axes[0]:
            ax.set_yticks([])

    axes[0].set_ylabel(r'$\log_{10}(r)$')

    fig.subplots_adjust(left=0.05, right=0.95, top=0.95, bottom=0.05, wspace=0.1, hspace=0.0)
    fig.suptitle(filename)

    return fig



def plot_radial_profile(filename):
    fig = plt.figure(figsize=[12, 8])
    ax1 = fig.add_subplot(4, 1, 1)
    ax2 = fig.add_subplot(4, 1, 2)
    ax3 = fig.add_subplot(4, 1, 3)
    ax4 = fig.add_subplot(4, 1, 4)

    h5f = h5py.File(filename, 'r')
    ja = 0
    jb = np.argmin(abs(h5f['polar_vertices'][...] - 0.1))
    jc = np.argmin(abs(h5f['polar_vertices'][...] - 0.2))

    rva = h5f['radial_vertices'][...] / 1e10
    u0a = h5f['radial_gamma_beta'][:,ja]
    L0a = h5f['radial_energy_flow'][:,ja]
    p0a = h5f['gas_pressure'][:,ja]
    d0a = h5f['mass_density'][:,ja]

    rvb = h5f['radial_vertices'][...] / 1e10
    u0b = h5f['radial_gamma_beta'][:,jb]
    L0b = h5f['radial_energy_flow'][:,jb]
    p0b = h5f['gas_pressure'][:,jb]
    d0b = h5f['mass_density'][:,jb]

    rvc = h5f['radial_vertices'][...] / 1e10
    u0c = h5f['radial_gamma_beta'][:,jc]
    L0c = h5f['radial_energy_flow'][:,jc]
    p0c = h5f['gas_pressure'][:,jc]
    d0c = h5f['mass_density'][:,jc]

    ax1.plot(rva[1:], u0a, lw=2.0, label=r'$\theta=0.0$')
    ax2.plot(rva[1:], L0a, lw=2.0, label=r'$\theta=0.0$')
    ax3.plot(rva[1:], p0a, lw=2.0, label=r'$\theta=0.0$')
    ax4.plot(rva[1:], d0a, lw=2.0, label=r'$\theta=0.0$')

    ax1.plot(rvb[1:], u0b, lw=2.0, label=r'$\theta=0.1$')
    ax2.plot(rvb[1:], L0b, lw=2.0, label=r'$\theta=0.1$')
    ax3.plot(rvb[1:], p0b, lw=2.0, label=r'$\theta=0.1$')
    ax4.plot(rvb[1:], d0b, lw=2.0, label=r'$\theta=0.0$')

    ax1.plot(rvc[1:], u0c, lw=2.0, label=r'$\theta=0.2$')
    ax2.plot(rvc[1:], L0c, lw=2.0, label=r'$\theta=0.2$')
    ax3.plot(rvc[1:], p0c, lw=2.0, label=r'$\theta=0.2$')
    ax4.plot(rvc[1:], d0a, lw=2.0, label=r'$\theta=0.2$')

    ax1.set_yscale('log')
    ax2.set_yscale('log')
    ax3.set_yscale('log')
    ax4.set_yscale('log')

    ax1.set_ylabel(r'$\Gamma \beta_r$')
    ax2.set_ylabel(r'$dL / d\Omega {\rm (erg/s/Sr)}$')
    ax3.set_ylabel(r'Gas Pressure ${\rm (erg/cm^3)}$')
    ax4.set_ylabel(r'Mass Density ${\rm (g/cm^3)}$')
    ax4.set_xlabel(r'Radius ${\rm (10^{{10}} cm)}$')

    ax1.legend()
    ax2.legend()
    ax3.legend()



if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('filenames', nargs='+')
    parser.add_argument('--radial', action='store_true')
    args = parser.parse_args()

    for filename in args.filenames:
        if args.radial:
            plot_radial_profile(filename)
        else:
            plot_single_file(filename)
    plt.show()
