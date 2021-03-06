#include "meshgenerator.h"

//------------------------------------------------------------------------------
mg::MeshGenerator::MeshGenerator()
{
}
//------------------------------------------------------------------------------
mg::MeshGenerator::MeshGenerator(mg::Parameters parameters):
    param(parameters)
{
    image = CImg<double> (parameters.imgPath.c_str());

    h = image.height();
    w = image.width();
    img_data = arma::mat(h, w);
    for(int j=0;j<w;j++) {
        for(int i=0;i<h;i++) {
            img_data(i,j) = image(j,i,0,0)/(255.0);
        }
    }

    n = parameters.nParticles;
    q = parameters.q;
    threshold = parameters.threshold;

    alpha_1 = parameters.alpha_1;
    alpha_2 = parameters.alpha_2;
    beta_1 = parameters.beta_1;
    beta_2 = parameters.beta_2;
    
    if(! parameters.setBoundaries) {
        X_0 = 0;
        X_1 = (double)w/w;
        Y_0 = 0;
        Y_1 = (double)h/w;
    } else {
        X_0 = parameters.X_0;
        X_1 = parameters.X_1;
        Y_0 = parameters.Y_0;
        Y_1 = parameters.Y_1;
    }
    x = arma::randu(2,n);
    js = arma::ones(n);

    seed = std::chrono::system_clock::now().time_since_epoch().count();
    generator = std::default_random_engine(seed);
    distribution_x = std::uniform_real_distribution<double> (X_0, X_1);
    distribution_y = std::uniform_real_distribution<double> (Y_0, Y_1);

    dx  = (X_1 - X_0)/w;
    dy  = (Y_1 - Y_0)/h;
    DX = (X_1 - X_0);
    DY = (Y_1 - Y_0);

    periodic_x = parameters.periodic_x;
    periodic_y = parameters.periodic_y;
    saveImage = parameters.saveImage;

    imageResolution = parameters.imageResolution;
    basePath = parameters.basePath;
    testSaveFreq = parameters.testSaveFreq;

    setDomainSize(2.01);

    openmp_threads = parameters.openmp_threads;
}
//------------------------------------------------------------------------------
void mg::MeshGenerator::initializeFromImage()
{
    // Randomly trying points within the image.
    //#pragma omp parallel for
    for(int i=0; i<n; i++)
    {
        arma::vec2 r_i = x.col(i);
        do
        {
            r_i[0] = distribution_x(generator);
            r_i[1] = distribution_y(generator);
        }while(img_data(r_i(1)/dy, r_i(0)/dx) > 0);
        x.col(i) = r_i;
    }

    std::cout << "Initialization from image complete" << std::endl;
}
//------------------------------------------------------------------------------
arma::mat mg::MeshGenerator::createMesh()
{
    createDomainGrid();
    initializeFromImage();

    std::uniform_real_distribution<double> distribution_rand_particle(X_0, n);
    // Sampling the image Monte Carlo style and adjusting the point centers
    // untill convergence.

//    vector<array<double, 3>> neighbours(n);
    vector<vector<double>> neighbours(n, vector<double>(3,1));


#ifdef FORCE_OMP_CPU
        omp_set_num_threads(openmp_threads);
#endif
#pragma omp parallel for
    for(int i=0; i<n; i++) {
        vector<double> &dun = neighbours[i];
        dun[0] = 0;
        dun[1] = 0;
        dun[2] = 0;
    }

    for (int k=0; k<threshold;k++) {
//        std::cout << "k = " << k << std::endl;
        printProgress(double(k)/threshold);

        checkBoundaries();
        mapParticlesToGrid();

        if(param.testingSave  && k % testSaveFreq == 0)
        {
            save_image_and_xyz(basePath + "/alg1", k);
            calculateRadialDistribution(k);
        }

        if(k % param.redistributionFrequency == 0)
        {
            // Picking nRandom points for redistribution
            for(int iterations=0; iterations < param.nRedistributedPoints; iterations++)
            {
                int random_particle = distribution_rand_particle(generator);
                do
                {
                    x(0, random_particle) = distribution_x(generator);
                    x(1, random_particle) = distribution_y(generator);
                }while(img_data(x(1, random_particle)/dy, x(0, random_particle)/dx) > 0);
            }
        }

#ifdef FORCE_OMP_CPU
        omp_set_num_threads(openmp_threads);
#endif
#pragma omp parallel for
        for(int r=0; r<q; r++) {
            double y_r[2];
            double maxLen = numeric_limits<double>::max();
            int indexMax = -1;

            do
            {
                y_r[0] = distribution_x(generator);
                y_r[1] = distribution_y(generator);
            }while(img_data(y_r[1]/dy, y_r[0]/dx) > 0);

            arma::vec2 y_t = y_r;
            int gId = findGridId(y_t);
            double y_tmp[2];
            y_tmp[0] = y_t(0);
            y_tmp[1] = y_t(1);

            // Finding the closest voronoi center
            //------------------------------------------------------------------
            // Checking this gridpoint
            //------------------------------------------------------------------

            double x_k[2];
            double y_r_copy[2];
            for(int k:particlesInGridPoint[gId])
            {
                y_r_copy[0] = y_r[0];
                y_r_copy[1] = y_r[1];
                x_k[0] = y_r[0] - x(0, k);
                x_k[1] = y_r[1] - x(1, k);

                if(periodic_x)
                {
                    if(x_k[0] > 0.5*DX){
                        x_k[0] -= DX;
                        y_r_copy[0] -= DX;
                    }else if(x_k[0] < -0.5*DX){
                        x_k[0] += DX;
                        y_r_copy[0] += DX;
                    }
                }

                if(periodic_y)
                {
                    if(x_k[1] > 0.5*DY){
                        x_k[1] -= DY;
                        y_r_copy[1] -= DY;
                    }else if(x_k[1] < -0.5*DY){
                        x_k[1] += DY;
                        y_r_copy[1] += DY;
                    }
                }

                double dr_rk = x_k[0]*x_k[0] + x_k[1]*x_k[1];

                if(dr_rk < maxLen)
                {
                    maxLen = dr_rk;
                    indexMax = k;
                    y_tmp[0] = y_r_copy[0];
                    y_tmp[1] = y_r_copy[1];
                }
            }

            //------------------------------------------------------------------
            // Checking neighbouring gridpoint
            //------------------------------------------------------------------
            for(int gridNeighbour:gridNeighbours[gId])
            {
                for(int k:particlesInGridPoint[gridNeighbour])
                {
                    y_r_copy[0] = y_r[0];
                    y_r_copy[1] = y_r[1];
                    x_k[0] = y_r[0] - x(0, k);
                    x_k[1] = y_r[1] - x(1, k);

                    if(periodic_x)
                    {
                        if(x_k[0] > 0.5*DX){
                            x_k[0] -= DX;
                            y_r_copy[0] -= DX;
                        }else if(x_k[0] < -0.5*DX){
                            x_k[0] += DX;
                            y_r_copy[0] += DX;
                        }
                    }

                    if(periodic_y)
                    {
                        if(x_k[1] > 0.5*DY){
                            x_k[1] -= DY;
                            y_r_copy[1] -= DY;
                        }else if(x_k[1] < -0.5*DY){
                            x_k[1] += DY;
                            y_r_copy[1] += DY;
                        }
                    }

                    double dr_rk = x_k[0]*x_k[0] + x_k[1]*x_k[1];

                    if(dr_rk < maxLen)
                    {
                        maxLen = dr_rk;
                        indexMax = k;
                        y_tmp[0] = y_r_copy[0];
                        y_tmp[1] = y_r_copy[1];
                    }
                }
            }
            //------------------------------------------------------------------
            // Storing the result
#pragma omp critical
            {
                if(indexMax >= 0) {
                    vector<double> &du = neighbours[indexMax];
                    du[0] += y_tmp[0];
                    du[1] += y_tmp[1];
                    du[2] += 1;
                }
            }
        }

#ifdef FORCE_OMP_CPU
        omp_set_num_threads(openmp_threads);
#endif
#pragma omp parallel for
        for(int i=0; i<n; i++) {
            vector<double> &dun = neighbours[i];

            if(dun[2] <= 0)
                continue;
            double j = js(i);
            arma::vec2 x_i =  x.col(i);
            arma::vec2 u_r;
            u_r[0] = dun[0];
            u_r[1] = dun[1];

            u_r /= dun[2];

            x.col(i) = ((alpha_1*j + beta_1)*x_i + (alpha_2*j + beta_2)*u_r)/(j+1);
            js(i) += 1;

            dun[0] = 0;
            dun[1] = 0;
            dun[2] = 0;
        }
    }

    return x;
}
//------------------------------------------------------------------------------
void mg::MeshGenerator::createDomainGrid()
{
    std::vector<int> pluss_minus = {-1, 0, 1};
    gridNeighbours = std::vector<vector<int>> (nx*ny, std::vector<int>(0));
    particlesInGridPoint.clear();
    particlesInGridPoint = std::vector<vector<int>>(nx*ny, std::vector<int>(0));

    for(int i=0;i<nx;i++)
    {
        for(int j=0;j<ny;j++)
        {
            int id_ij = j + ny*i;

            for(int id_x:pluss_minus)
            {
                id_x += i;
                if(id_x < 0)
                {
                    if(periodic_x)
                        id_x = nx-1;
                    else
                        continue;
                }
                if(id_x >= nx)
                {
                    if(periodic_x)
                        id_x = 0;
                    else
                        continue;
                }

                for(int id_y:pluss_minus)
                {
                    id_y += j;
                    if(id_y < 0)
                    {
                        if(periodic_y)
                            id_y = ny-1;
                        else
                            continue;
                    }
                    if(id_y >= ny)
                    {
                        if(periodic_y)
                            id_y = 0;
                        else
                            continue;
                    }
                    int id_neighbour = id_y + ny*id_x;
                    if(id_neighbour == id_ij)
                        continue;

                    gridNeighbours[id_ij].push_back(id_neighbour);
                }
            }
        }
    }

//    int c = 0;
//    for(std::vector<int> t:gridNeighbours)
//    {
//        cout << "c = " << c << endl;
//        for(int b:t)
//        {
//            cout << b << " ";
//        }
//        cout << endl;
//        c++;
//    }
//    cout << "done " << endl;
}
//------------------------------------------------------------------------------
void mg::MeshGenerator::mapParticlesToGrid()
{
    // Emptying the grid
    for(vector<int> &t:particlesInGridPoint)
    {
        t.clear();
    }

    // Placing all particles in the grid
#ifdef FORCE_OMP_CPU
        omp_set_num_threads(openmp_threads);
#endif
#pragma omp parallel for
    for(int i=0; i<n; i++)
    {
        const arma::vec2 & r_i = x.col(i);
#pragma omp critical
        particlesInGridPoint[findGridId(r_i)].push_back(i);
    }

//    // Printing results for debugging
//    int counter = 0;
//    int nParticles = 0;
//    for(vector<int> t:particlesInGridPoint)
//    {
//        std::cout << "gridPoint " << counter << std::endl;
//        nParticles += t.size();
//        for(int i:t)
//        {
//            std::cout << i << " ";
//        }
//        std::cout << std::endl;
//        counter++;
//    }
//    std::cout << "nParticles " << nParticles << std::endl;
//    std::cout << "done " << std::endl;
}
//------------------------------------------------------------------------------
void mg::MeshGenerator::save_image_and_xyz(string base, int nr)
{
    // Bounds check
    checkBoundaries();
    mapParticlesToGrid();

    string fileName;
    if(nr == -1)
        fileName = base + ".pgm";
    else
        fileName = base + "_" + to_string(nr) + ".pgm";
    arma::vec areas = arma::zeros(n);

    int resolution_x = X_1*imageResolution;
    int resolution_y = Y_1*imageResolution;
    arma::mat image(resolution_y, resolution_x);
    image.zeros();

    int pix_hole = 0;
    //--------------------------------------------------------------------------
    // Creating a Voronoi image and computing the areas
    //--------------------------------------------------------------------------
#ifdef FORCE_OMP_CPU
        omp_set_num_threads(openmp_threads);
#endif
#pragma omp parallel for
    for (int i=0; i<resolution_x;i++)
    {
        for (int j=0; j<resolution_y;j++)
        {
            arma::vec2 r_img;
            r_img[0] = X_1*i/(resolution_x);
            r_img[1] = Y_1*j/(resolution_y);

            double maxLen = numeric_limits<double>::max();
            int indexMax = -1;

            if(img_data(r_img(1)/dy, r_img(0)/dx) > 0){
                image(j, i) = 0;
                pix_hole++;
                continue;
            }

            int gId = findGridId(r_img);

            // Finding the closest voronoi center
            //------------------------------------------------------------------
            // Checking this gridpoint
            //------------------------------------------------------------------
            for(int k:particlesInGridPoint[gId])
            {
                arma::vec2 x_k = r_img - x.col(k);

                if(periodic_x)
                {
                    if(x_k(0) > 0.5*DX){
                        x_k(0) -= DX;
                    }else if(x_k(0) < -0.5*DX){
                        x_k(0) += DX;
                    }
                }

                if(periodic_y)
                {
                    if(x_k(1) > 0.5*DY){
                        x_k(1) -= DY;
                    }else if(x_k(1) < -0.5*DY){
                        x_k(1) += DY;
                    }
                }

                double dr_rk = x_k(0)*x_k(0) + x_k(1)*x_k(1);

                if(dr_rk < maxLen)
                {
                    maxLen = dr_rk;
                    indexMax = k;
                }
            }

            //------------------------------------------------------------------
            // Checking neighbouring gridpoint
            //------------------------------------------------------------------
            for(int gridNeighbour:gridNeighbours[gId])
            {
                for(int k:particlesInGridPoint[gridNeighbour])
                {
                    arma::vec2 x_k = r_img - x.col(k);

                    if(periodic_x)
                    {
                        if(x_k(0) > 0.5*DX){
                            x_k(0) -= DX;
                        }else if(x_k(0) < -0.5*DX){
                            x_k(0) += DX;
                        }
                    }

                    if(periodic_y)
                    {
                        if(x_k(1) > 0.5*DY){
                            x_k(1) -= DY;
                        }else if(x_k(1) < -0.5*DY){
                            x_k(1) += DY;
                        }
                    }

                    double dr_rk = x_k(0)*x_k(0) + x_k(1)*x_k(1);

                    if(dr_rk < maxLen)
                    {
                        maxLen = dr_rk;
                        indexMax = k;
                    }
                }
            }

            if(indexMax != -1)
            {
                image(j, i) = indexMax;
                areas[indexMax] += 1.0;
            }else
            {
                image(j, i) = 0;
            }
        }
    }

    // Adding the voronoi centers to the image
    for (int k=0;k<n; k++)
        image(x(1, k)*resolution_y/Y_1, x(0, k)*resolution_x) = 1.0;

    // Saving the voronoi image
    if(saveImage)
        image.save(fileName, arma::pgm_binary);

    //--------------------------------------------------------------------------
    // Saving xyz-file with volume
    //--------------------------------------------------------------------------
    if(nr == -1)
        fileName =  base + ".xyz";
    else
        fileName =  base + "_" + to_string(nr) + ".xyz";
    ofstream outStream(fileName.c_str());

    outStream << x.n_cols << endl;
    outStream << "# id x y z volume" << endl;
    double dxdy = (X_1 - X_0)*(Y_1 - Y_0);
    double height = 1.0;
    double s_volume = 0;
//    double total_pix = resolution_x*resolution_y - pix_hole;
    double total_pix = resolution_x*resolution_y;
//    double optimalPackingOfCircles = 0.907;
    double optimalPackingOfCircles = 1.0    ;
    for (int i=0; i<n;i++)
    {
        double volume = optimalPackingOfCircles*dxdy*height* areas[i]/(total_pix);
        const arma::vec2& r = x.col(i);
        outStream << i << "\t" << r[0] << "\t" << r[1] << "\t"<< " 0 " << " " << volume << std::endl;
        s_volume += volume;
    }
    outStream.close();
    cout << fileName << endl;
}
//------------------------------------------------------------------------------
void mg::MeshGenerator::setDomainSize(double spacing)
{
    // Setting the grid size
    double rho = n/(DX*DY);
    nx = sqrt(rho)*DX;
    ny = sqrt(rho)*DY;

    double gridSpacing = spacing*DX/nx;

    nx = floor((DX)/gridSpacing);
    ny = floor((DY)/gridSpacing);

    gridSpacing_x = DX/nx;
    gridSpacing_y = DY/ny;

    if(nx == 0)
        nx = 1;
    if(ny == 0)
        ny = 1;
}
//------------------------------------------------------------------------------
double mg::MeshGenerator::calculateRadialDistribution(int nr)
{
    std::cout << "Calculating histogram" << std::endl;
    setDomainSize(4.01);
    createDomainGrid();
    checkBoundaries();
    mapParticlesToGrid();

    int nBins = 300;
    double maxLength = 1.6*gridSpacing_x;
    double histSpacing = maxLength/nBins;
    vector<int> histogram(nBins, 0);

#ifdef FORCE_OMP_CPU
        omp_set_num_threads(openmp_threads);
#endif
#pragma omp parallel for
    for(int i=0; i<n; i++)
    {
        const arma::vec2 & r_i = x.col(i);
        int gId = findGridId(r_i);

        //------------------------------------------------------------------
        // Checking this gridpoint
        //------------------------------------------------------------------

        for(int k:particlesInGridPoint[gId])
        {
            if(k == i)
                continue;
            arma::vec2 r_ij = r_i - x.col(k);

            if(periodic_x)
            {
                if(r_ij(0) > 0.5*DX){
                    r_ij(0) -= DX;
                }else if(r_ij(0) < -0.5*DX){
                    r_ij(0) += DX;
                }
            }

            if(periodic_y)
            {
                if(r_ij(1) > 0.5*DY){
                    r_ij(1) -= DY;
                }else if(r_ij(1) < -0.5*DY){
                    r_ij(1) += DY;
                }
            }

            double dr_rk = sqrt(r_ij(0)*r_ij(0) + r_ij(1)*r_ij(1));
            if(dr_rk > maxLength)
                continue;
            int id = dr_rk/histSpacing;
            histogram[id]++;
        }

        //------------------------------------------------------------------
        // Checking neighbouring gridpoint
        //------------------------------------------------------------------
        for(int gridNeighbour:gridNeighbours[gId])
        {
            for(int k:particlesInGridPoint[gridNeighbour])
            {
                arma::vec2 r_ij = r_i - x.col(k);

                if(periodic_x)
                {
                    if(r_ij(0) > 0.5*DX){
                        r_ij(0) -= DX;
                    }else if(r_ij(0) < -0.5*DX){
                        r_ij(0) += DX;
                    }
                }

                if(periodic_y)
                {
                    if(r_ij(1) > 0.5*DY){
                        r_ij(1) -= DY;
                    }else if(r_ij(1) < -0.5*DY){
                        r_ij(1) += DY;
                    }
                }

                double dr_rk = sqrt(r_ij(0)*r_ij(0) + r_ij(1)*r_ij(1));
                if(dr_rk > maxLength)
                    continue;
                int id = dr_rk/histSpacing;
                histogram[id]++;
            }
        }
        //------------------------------------------------------------------
    }
    string fileName;
    if(nr == -1)
        fileName = basePath + "/histogram.hist";
    else
        fileName = basePath + "/histogram_" + to_string(nr) + ".hist";
    ofstream outStream(fileName.c_str());


    for(uint i=1;i<histogram.size(); i++)
    {
        double r1 = i*histSpacing;
        double r2 = r1 + histSpacing;
        outStream << r1 + 0.5*histSpacing << "\t" << histogram[i]/(M_PI*(pow(r2,2) - pow(r1,2))) << std::endl;
    }
    outStream.close();

    // Finding the optimal spacing between the particles
    int maxIndex = -1;
    double maxValue = 0;
    optimalGridSpacing = 0;

    for(uint i=1;i<histogram.size(); i++)
    {
        double r1 = i*histSpacing;
        double r2 = r1 + histSpacing;
        double hist_i = histogram[i]/(M_PI*(pow(r2,2) - pow(r1,2)));
        if(hist_i > maxValue)
        {
            maxIndex = i;
            maxValue = hist_i;
        }
    }

    // Resetting the grid
    setDomainSize(2.01);
    createDomainGrid();
    mapParticlesToGrid();

    optimalGridSpacing = (maxIndex + 0.5)*histSpacing;
    return optimalGridSpacing;
}
//------------------------------------------------------------------------------
void mg::MeshGenerator::writeConfiguration()
{
    std::cout << "Writing configuration" << std::endl;
    string fileName = basePath + "/configuration.cfg";
    ofstream outStream(fileName.c_str());
    outStream.setf(ios::scientific);
    outStream.precision(5);

    outStream << "nParticles = " << n << std::endl;
    outStream << "spacing = " << optimalGridSpacing << std::endl;
    int n_x = floor((X_1 - X_0)/optimalGridSpacing);
    int n_y = floor((Y_1 - Y_0)/optimalGridSpacing);
    outStream << "latticePoints = [" << n_x<< ", " << n_y << ", " << "1]"
              << std::endl;
    outStream << "boundaries = [" << X_0 << ", " << X_1 <<  ", "
              << Y_0 << ", " << Y_1 << ", "
              << -0.5*optimalGridSpacing << ", " << 0.5*optimalGridSpacing << "]"
              << std::endl;

    outStream << "periodic = [";
    if(periodic_x)
        outStream << "1, ";
    else
        outStream << "0, ";

    if(periodic_y)
        outStream << "1, ";
    else
        outStream << "0, ";


    outStream << "0]" << std::endl;

    outStream.close();
}
//------------------------------------------------------------------------------
int mg::MeshGenerator::findGridId(const arma::vec2 &r)
{
    int id_x = (r(0) - X_0)/gridSpacing_x;
    int id_y = (r(1) - Y_0)/gridSpacing_y;

    // Boundary checks
    if(id_x > nx)
        id_x = nx - 1;
    else if(id_x < 0)
        id_x = 0;

    if(id_y > ny)
        id_y = ny - 1;
    else if(id_y < 0)
        id_y = 0;

    return id_y + ny*id_x;
}
//------------------------------------------------------------------------------
void mg::MeshGenerator::checkBoundaries()
{
    if(periodic_x)
    {
#pragma omp parallel for
        for(int k=0;k<n; k++)
        {
            if(x(0,k) < X_0)
                x(0,k) += DX;
            if(x(0,k) >= X_1)
                x(0,k) -= DX;
        }
    }

    if(periodic_y)
    {
#pragma omp parallel for
        for(int k=0;k<n; k++)
        {
            if(x(1,k) < Y_0)
                x(1,k) += DY;
            if(x(1,k) >= Y_1)
                x(1,k) -= DY;
        }
    }
}

//------------------------------------------------------------------------------
void mg::MeshGenerator::printProgress(double progress)
{
    int barWidth = 70;

    std::cout << "[";
    int pos = barWidth * progress;
    for (int j = 0; j < barWidth; ++j) {
        if (j < pos)
            std::cout << "=";
        else if (j == pos)
            std::cout << ">";
        else
            std::cout << " ";
    }
    std::cout << "] " << int(progress * 100.0) << " %\r";
    std::cout.flush();
}
//------------------------------------------------------------------------------
