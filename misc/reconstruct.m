% This script is tailored for the 9th level of the CMU-1 data set.
% The files input should be a cell array of string pointing to the
% two JPEG files of which the 9th is build up. You may use split-mirax.py
% to extract all images from the CMU-1 data set.
function reconstructed_image = reconstruct(files,cvt_to_int)

    % Check whether there is a specific rounding mode requested.
    if (nargin==1)
        cvt_to_int = @(x)round(x);
    end

    % Define how many tile images there are and read them.
    num_images = length(files);
    tile_images = cell(num_images,1);

    for img_idx=1:num_images
        tile_images{img_idx} = imread(files{img_idx});
    end

    % Store the size of the tile images.
    [height width channels] = size(tile_images{1});

    % Fix some parameters required for the reconstruction.
    pyramid_level = 9;
    level_0_overlap = 120;
    num_tiles_per_dim = 2^(pyramid_level-2);

    % float tile width in pixels
    tiles.width = width/num_tiles_per_dim;
    % float tile height in pixels
    tiles.height = height/num_tiles_per_dim;

    % If we do not ceil but floor here, we get missing data!

    % ceiled tile indices, i.e. 2.65... => 3 and the indices [1 2 3]
    tiles.indices_x = 1:ceil(tiles.width);
    % ceiled tile indices, i.e. 1.23... => 2 and the indices [1 2]
    tiles.indices_y = 1:ceil(tiles.height);

    % compute the tile overlap for the current level
    tiles.overlap = level_0_overlap / 2^pyramid_level;

    % allocate memory for the output image
    reconstructed_image = 255*ones(512,256,3,'uint8');

    for img_idx=1:2
        for tile_idx_y=1:num_tiles_per_dim
            for tile_idx_x=1:num_tiles_per_dim

                % The jpeg image containing all tiles
                tile_image = tile_images{img_idx};

                % - We iterate over all tiles in the jpeg image and need to
                % compute their pixel offsets, i.e. the pixel index of the
                % upper left corner of a tile.
                % - Because the tile width and height are actually floating
                % point values we need some sort of conversion to integer
                % over here.
                % - All tile offeset computed in the following two lines
                % should be C compatible (zero based indices)
                tile_off_x = cvt_to_int((tile_idx_x-1)*tiles.width);
                tile_off_y = cvt_to_int((tile_idx_y-1)*tiles.height);

                % Here, we are just computing the actual pixel indices.
                % These indices are now Matlab style (one based)
                tile_indices_x = tile_off_x + tiles.indices_x;
                tile_indices_y = tile_off_y + tiles.indices_y;

                % Next, we need to compute where to paste the tile in the
                % image. We do this in three steps.
                % 1. Compute the numerical correct values in one image.
                img_off_x = (tile_idx_x-1)*(tiles.width-tiles.overlap);
                img_off_y = (tile_idx_y-1)*(tiles.height-tiles.overlap);

                % 2. Add the vertical offset for JPEG images above and left
                % to the current image. In this particular example, there
                % are only images above.
                % The number of pixels in the original image that is
                % covered by a single JPEG file. I.e. number of tiles times
                % tile height minus tile overlaps
                jpeg_height = num_tiles_per_dim*(tiles.height) - ...
                    (num_tiles_per_dim-1)*tiles.overlap;
                img_off_y = img_off_y + (img_idx-1)*jpeg_height;

                % 3. Again, we need some rounding
                img_off_x = cvt_to_int(img_off_x);
                img_off_y = cvt_to_int(img_off_y);

                % ... and again, we need the actual Matlab compatible image
                % indices.
                img_indices_x = img_off_x + tiles.indices_x;
                img_indices_y = img_off_y + tiles.indices_y;

                if (max(tile_indices_x(:)) > width), continue; end
                if (max(tile_indices_y(:)) > height), continue; end

                % extract the current tile
                tile = tile_image(tile_indices_y, tile_indices_x, :);

                % and paste it to the destination image
                reconstructed_image(img_indices_y, img_indices_x, :) = tile;

            end
        end
    end
end
